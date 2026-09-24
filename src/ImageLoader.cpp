#include "ImageLoader.h"
#include "FileIo.h"
#include "TurboJpeg.h"

#include "ExifOrientation.h"
#include "IccProfile.h"
#include "ImageTransform.h"

#include <shlwapi.h>  // SHCreateMemStream

#include <cmath>
#include <cstdlib>
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

// XMP property Apple writes on the gain map: the HDR rendition's peak over
// SDR white, as a linear ratio.
constexpr wchar_t kAppleHeadroomQuery[] =
    L"/xmp/http\\:\\/\\/ns.apple.com\\/HDRGainMap\\/1.0\\/:HDRGainMapHeadroom";

float ReadAppleHeadroom(IWICBitmapFrameDecode* frame)
{
    wil::com_ptr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.put())))
    {
        return 0.0f;
    }
    wil::unique_prop_variant value;
    if (FAILED(reader->GetMetadataByName(kAppleHeadroomQuery, &value)))
    {
        return 0.0f;
    }
    switch (value.vt)
    {
    case VT_LPWSTR:
        return std::wcstof(value.pwszVal, nullptr);
    case VT_LPSTR:
        return std::strtof(value.pszVal, nullptr);
    case VT_R4:
        return value.fltVal;
    case VT_R8:
        return static_cast<float>(value.dblVal);
    default:
        return 0.0f;
    }
}

HRESULT DecodeToBgra8(IWICImagingFactory* factory, IWICBitmapSource* source, PixelBuffer& out)
{
    wil::com_ptr<IWICFormatConverter> converter;
    RETURN_IF_FAILED(factory->CreateFormatConverter(converter.put()));
    RETURN_IF_FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom));
    UINT width = 0;
    UINT height = 0;
    RETURN_IF_FAILED(converter->GetSize(&width, &height));
    RETURN_HR_IF(WINCODEC_ERR_BADIMAGE, width == 0 || height == 0);
    const uint64_t stride64 = uint64_t{width} * 4;
    RETURN_HR_IF(kHrImageTooLarge, stride64 * height > kMaxPixelBytes);

    out.format = PixelBuffer::Format::Bgra8;
    out.width = width;
    out.height = height;
    out.stride = static_cast<uint32_t>(stride64);
    out.pixels.resize(static_cast<size_t>(stride64 * height));
    return converter->CopyPixels(nullptr, out.stride, static_cast<UINT>(out.pixels.size()),
                                 out.pixels.data());
}

// The first ICC profile among the frame's color contexts (JPEG can list an
// EXIF color space as well), or empty.
std::vector<uint8_t> ReadWicIccProfile(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame)
{
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0)
    {
        return {};
    }
    std::vector<wil::com_ptr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i)
    {
        if (FAILED(factory->CreateColorContext(contexts[i].put())))
        {
            return {};
        }
        raw[i] = contexts[i].get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count)))
    {
        return {};
    }
    for (UINT i = 0; i < count; ++i)
    {
        WICColorContextType type{};
        UINT size = 0;
        if (FAILED(raw[i]->GetType(&type)) || type != WICColorContextProfile
            || FAILED(raw[i]->GetProfileBytes(0, nullptr, &size)) || size == 0)
        {
            continue;
        }
        std::vector<uint8_t> profile(size);
        if (SUCCEEDED(raw[i]->GetProfileBytes(size, profile.data(), &size)))
        {
            profile.resize(size);
            return profile;
        }
    }
    return {};
}

// Decides what to do with an embedded profile. Only 8-bit pixels are
// converted for now; high-precision ones were already linearized as sRGB.
// See DESIGN.md, "Color profiles".
ColorProfile MakeColorProfile(std::vector<uint8_t> icc, PixelBuffer::Format format)
{
    ColorProfile profile;
    if (icc.empty())
    {
        return profile;
    }
    profile.description = IccDescription(icc);
    if (!IsRgbProfile(icc))
    {
        profile.handling = ColorProfile::Handling::Unsupported;
        return profile;
    }
    if (IsSrgbEquivalent(icc))
    {
        profile.handling = ColorProfile::Handling::NotNeeded;
    }
    else
    {
        profile.handling = format == PixelBuffer::Format::Bgra8
                               ? ColorProfile::Handling::Converted
                               : ColorProfile::Handling::Unsupported;
    }
    profile.icc = std::move(icc);
    return profile;
}

// Attaches Apple's HDR gain map (iPhone HEIC) when the codec exposes one
// through a gain-map frame chain. Codecs or HEIF extensions without that
// support, or a map without its headroom, leave the image SDR exactly as
// before. See DESIGN.md, "HDR gain map".
void TryAttachGainMap(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                      LoadedImage& out) noexcept
try
{
    const auto chain = wil::try_com_query<IWICBitmapFrameChainReader>(frame);
    UINT count = 0;
    if (!chain || FAILED(chain->GetChainedFrameCount(WICBitmapChainType_GainMap, &count))
        || count == 0)
    {
        return;
    }
    wil::com_ptr<IWICBitmapFrameDecode> gainFrame;
    THROW_IF_FAILED(chain->GetChainedFrame(WICBitmapChainType_GainMap, 0, gainFrame.put()));

    float headroom = ReadAppleHeadroom(gainFrame.get());
    if (!(headroom > 1.0f))
    {
        headroom = ReadAppleHeadroom(frame);
    }
    if (!(headroom > 1.0f) || !std::isfinite(headroom))
    {
        return;  // not Apple's format, or nothing to gain
    }

    HdrGainMap gainMap;
    THROW_IF_FAILED(DecodeToBgra8(factory, gainFrame.get(), gainMap.map));
    gainMap.headroom = headroom;
    out.gainMap = std::move(gainMap);
}
CATCH_LOG()

// TIFF's Orientation through WIC's photo metadata policy (EXIF, then XMP).
uint16_t ReadTiffOrientation(IWICBitmapFrameDecode* frame)
{
    wil::com_ptr<IWICMetadataQueryReader> reader;
    wil::unique_prop_variant value;
    if (FAILED(frame->GetMetadataQueryReader(reader.put()))
        || FAILED(reader->GetMetadataByName(L"System.Photo.Orientation", &value))
        || value.vt != VT_UI2 || value.uiVal < 1 || value.uiVal > 8)
    {
        return 1;
    }
    return value.uiVal;
}

// Bakes an EXIF Orientation into the pixels (and any gain map), so display,
// editing, the cache and saving all see the upright image. See DESIGN.md,
// "EXIF Orientation".
void ApplyOrientation(LoadedImage& image, uint16_t orientation)
{
    const OrientationSteps steps = StepsForOrientation(orientation);
    if (steps.IsIdentity())
    {
        return;
    }
    if (steps.flipHorizontal)
    {
        FlipImageHorizontal(image);
    }
    if (steps.flipVertical)
    {
        FlipImageVertical(image);
    }
    if (steps.quarterTurnsClockwise != 0)
    {
        image = RotateImage90(image, steps.quarterTurnsClockwise == 1);
    }
    image.appliedOrientation = orientation;
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

    // Neither decoder applies the EXIF Orientation; JPEG's is read here,
    // from the bytes, because the libjpeg-turbo path has no WIC decoder.
    // HEIF needs nothing: its decoder already applies the container's
    // rotation, and applying EXIF on top would turn the image twice.
    const bool isJpeg = TurboJpeg::LooksLikeJpeg(data.data(), data.size());
    const uint16_t jpegOrientation = isJpeg ? ReadJpegExifOrientation(data) : 1;
    // Likewise JPEG's ICC profile, so both decoders see the same one.
    std::vector<uint8_t> jpegIcc = isJpeg ? ReadJpegIccProfile(data) : std::vector<uint8_t>{};

    // JPEG goes through the optional libjpeg-turbo codec when its DLL is
    // present; any failure there (CMYK, corrupt stream) falls back to WIC.
    if (isJpeg && TurboJpeg::IsAvailable()
        && SUCCEEDED(TurboJpeg::Decode(data.data(), data.size(), out)))
    {
        out.colorProfile = MakeColorProfile(std::move(jpegIcc), out.format);
        ApplyOrientation(out, jpegOrientation);
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

    // Gain maps lift an SDR base; high-precision sources carry their own range.
    if (out.format == LoadedImage::Format::Bgra8)
    {
        TryAttachGainMap(factory, frame.get(), out);
    }

    out.colorProfile = MakeColorProfile(
        isJpeg ? std::move(jpegIcc) : ReadWicIccProfile(factory, frame.get()), out.format);

    GUID container{};
    (void)decoder->GetContainerFormat(&container);
    ApplyOrientation(out, isJpeg                                     ? jpegOrientation
                          : container == GUID_ContainerFormatTiff ? ReadTiffOrientation(frame.get())
                                                                  : 1);
    return S_OK;
}
CATCH_RETURN()
