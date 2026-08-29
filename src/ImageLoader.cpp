#include "ImageLoader.h"
#include "FileIo.h"
#include "TurboJpeg.h"

#include <shlwapi.h>  // SHCreateMemStream

#include <utility>

namespace
{

// True when the source carries more than 8 bits per channel (16-bit ints,
// halfs, floats, or the 10-bit packed formats); those decode into half
// floats to preserve their range for the scRGB pipeline. An explicit list:
// deriving this from bits-per-pixel / channel count gets fooled by padding
// (32bppBGR is 32 bits for 3 channels but plain 8-bit).
bool IsHighPrecisionFormat(IWICImagingFactory* /*factory*/, const WICPixelFormatGUID& format)
{
    static const WICPixelFormatGUID kHighPrecision[] = {
        GUID_WICPixelFormat16bppGray,
        GUID_WICPixelFormat16bppGrayHalf,
        GUID_WICPixelFormat16bppGrayFixedPoint,
        GUID_WICPixelFormat32bppGrayFloat,
        GUID_WICPixelFormat32bppGrayFixedPoint,
        GUID_WICPixelFormat32bppBGR101010,
        GUID_WICPixelFormat32bppRGBA1010102,
        GUID_WICPixelFormat32bppRGBA1010102XR,
        GUID_WICPixelFormat32bppR10G10B10A2HDR10,
        GUID_WICPixelFormat48bppRGB,
        GUID_WICPixelFormat48bppBGR,
        GUID_WICPixelFormat48bppRGBHalf,
        GUID_WICPixelFormat48bppRGBFixedPoint,
        GUID_WICPixelFormat48bppBGRFixedPoint,
        GUID_WICPixelFormat64bppRGBA,
        GUID_WICPixelFormat64bppBGRA,
        GUID_WICPixelFormat64bppPRGBA,
        GUID_WICPixelFormat64bppPBGRA,
        GUID_WICPixelFormat64bppRGB,
        GUID_WICPixelFormat64bppRGBHalf,
        GUID_WICPixelFormat64bppRGBAHalf,
        GUID_WICPixelFormat64bppPRGBAHalf,
        GUID_WICPixelFormat64bppRGBFixedPoint,
        GUID_WICPixelFormat64bppRGBAFixedPoint,
        GUID_WICPixelFormat96bppRGBFloat,
        GUID_WICPixelFormat96bppRGBFixedPoint,
        GUID_WICPixelFormat128bppRGBFloat,
        GUID_WICPixelFormat128bppRGBAFloat,
        GUID_WICPixelFormat128bppPRGBAFloat,
        GUID_WICPixelFormat128bppRGBFixedPoint,
        GUID_WICPixelFormat128bppRGBAFixedPoint,
    };
    for (const auto& candidate : kHighPrecision)
    {
        if (format == candidate)
        {
            return true;
        }
    }
    return false;
}

}  // namespace

ImageLoader::ImageLoader(HWND notifyWindow, UINT notifyMessage)
    : m_notifyWindow(notifyWindow),
      m_notifyMessage(notifyMessage),
      m_thread([this](std::stop_token stopToken) { WorkerProc(stopToken); })
{
}

uint64_t ImageLoader::RequestLoad(std::filesystem::path path)
{
    uint64_t generation;
    {
        std::lock_guard lock(m_mutex);
        generation = m_nextGeneration++;
        m_pending = Request{generation, std::move(path)};
    }
    m_cv.notify_one();
    return generation;
}

uint64_t ImageLoader::RequestLoadFromMemory(std::vector<uint8_t> data,
                                            std::filesystem::path displayName)
{
    uint64_t generation;
    {
        std::lock_guard lock(m_mutex);
        generation = m_nextGeneration++;
        m_pending = Request{generation, std::move(displayName), std::move(data)};
    }
    m_cv.notify_one();
    return generation;
}

std::optional<ImageLoader::Result> ImageLoader::TakeResult()
{
    std::lock_guard lock(m_mutex);
    return std::exchange(m_completed, std::nullopt);
}

bool ImageLoader::ShouldAbort(const std::stop_token& stopToken)
{
    if (stopToken.stop_requested())
    {
        return true;
    }
    std::lock_guard lock(m_mutex);
    return m_pending.has_value();  // a newer request supersedes this one
}

void ImageLoader::WorkerProc(std::stop_token stopToken) noexcept
try
{
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    // The factory (and all WIC objects created from it) lives inside this
    // scope, below coInit, so everything is released before CoUninitialize.
    // WIC objects are never shared with other threads.
    auto factory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);

    while (true)
    {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, stopToken, [this] { return m_pending.has_value(); }))
            {
                return;  // stop requested
            }
            request = std::move(*m_pending);
            m_pending.reset();
        }

        Result result;
        result.generation = request.generation;
        result.path = request.path;
        result.hr = Decode(factory.get(), request, stopToken, result.image, result.animation);

        if (stopToken.stop_requested())
        {
            return;
        }
        if (result.hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            continue;  // superseded by a newer request; skip notification
        }

        const uint64_t generation = result.generation;
        {
            std::lock_guard lock(m_mutex);
            m_completed = std::move(result);  // an uncollected older result is dropped here
        }
        // Wake-up call only; ownership stays in the mailbox, so a failed post
        // (e.g. the window is already destroyed) cannot leak anything.
        PostMessageW(m_notifyWindow, m_notifyMessage, 0, static_cast<LPARAM>(generation));
    }
}
catch (...)
{
    // COM init or factory creation failed; pending loads will never complete.
    LOG_CAUGHT_EXCEPTION();
}

HRESULT ImageLoader::Decode(IWICImagingFactory* factory, Request& request,
                            const std::stop_token& stopToken, LoadedImage& out,
                            std::vector<AnimationFrame>& outAnimation) noexcept
try
{
    std::vector<uint8_t> data = std::move(request.data);
    if (data.empty())
    {
        // Read the file ourselves instead of letting WIC do the I/O: WIC's
        // file access has no cancellation point, while ReadFileChunked can
        // bail out between chunks on a slow OneDrive hydration or SMB read.
        wil::unique_hfile file(CreateFileW(request.path.c_str(), GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        RETURN_LAST_ERROR_IF(!file);

        LARGE_INTEGER fileSize{};
        RETURN_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &fileSize));
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE),
                     static_cast<uint64_t>(fileSize.QuadPart) > UINT_MAX);

        data.resize(static_cast<size_t>(fileSize.QuadPart));
        RETURN_IF_FAILED(
            ReadFileChunked(file.get(), data, [&] { return ShouldAbort(stopToken); }));
    }

    // JPEG goes through the optional libjpeg-turbo codec when its DLL is
    // present; any failure there (CMYK, corrupt stream) falls back to WIC.
    if (TurboJpeg::LooksLikeJpeg(data.data(), data.size()) && TurboJpeg::IsAvailable()
        && SUCCEEDED(TurboJpeg::Decode(data.data(), data.size(), out)))
    {
        return S_OK;
    }

    wil::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(data.data(), static_cast<UINT>(data.size())));
    RETURN_HR_IF_NULL(E_OUTOFMEMORY, stream.get());
    data = {};  // SHCreateMemStream copied the bytes; drop ours

    wil::com_ptr<IWICBitmapDecoder> decoder;
    RETURN_IF_FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                      WICDecodeMetadataCacheOnDemand,
                                                      decoder.put()));

    UINT frameCount = 1;
    (void)decoder->GetFrameCount(&frameCount);
    if (frameCount > 1)
    {
        // Any animation failure (odd metadata, over budget) falls back to
        // showing the first frame as a still image.
        std::vector<AnimationFrame> frames;
        if (SUCCEEDED(DecodeAnimation(factory, decoder.get(), frameCount, frames))
            && frames.size() > 1)
        {
            out = frames.front().image;
            outAnimation = std::move(frames);
            return S_OK;
        }
    }

    wil::com_ptr<IWICBitmapFrameDecode> frame;
    RETURN_IF_FAILED(decoder->GetFrame(0, frame.put()));

    // High-precision sources keep their range as half floats; everything
    // else takes the classic 8-bit sRGB path.
    WICPixelFormatGUID nativeFormat{};
    (void)frame->GetPixelFormat(&nativeFormat);
    const bool wantHalf = IsHighPrecisionFormat(factory, nativeFormat);
    out.format = wantHalf ? LoadedImage::Format::Rgba16F : LoadedImage::Format::Bgra8;
    const WICPixelFormatGUID targetFormat = out.WicPixelFormat();

    wil::com_ptr<IWICFormatConverter> converter;
    RETURN_IF_FAILED(factory->CreateFormatConverter(converter.put()));
    RETURN_IF_FAILED(converter->Initialize(frame.get(), targetFormat,
                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom));

    UINT width = 0;
    UINT height = 0;
    RETURN_IF_FAILED(converter->GetSize(&width, &height));
    RETURN_HR_IF(WINCODEC_ERR_BADIMAGE, width == 0 || height == 0);

    // 64-bit math: width/height come from the file and could overflow UINT.
    const uint64_t stride64 = uint64_t{width} * out.BytesPerPixel();
    const uint64_t total64 = stride64 * height;
    RETURN_HR_IF(kHrImageTooLarge, total64 > kMaxPixelBytes);

    out.width = width;
    out.height = height;
    out.stride = static_cast<uint32_t>(stride64);
    out.pixels.resize(static_cast<size_t>(total64));

    // CopyPixels performs the actual decode; not cancellable, but it operates
    // on in-memory data only, so it finishes in bounded time.
    RETURN_IF_FAILED(converter->CopyPixels(nullptr, out.stride,
                                           static_cast<UINT>(out.pixels.size()),
                                           out.pixels.data()));
    return S_OK;
}
CATCH_RETURN()
