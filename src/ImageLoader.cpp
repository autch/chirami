#include "ImageLoader.h"

#include <shlwapi.h>  // SHCreateMemStream

#include <algorithm>
#include <cstring>
#include <utility>

namespace
{

constexpr DWORD kReadChunkBytes = 1u << 20;  // 1 MiB

// Generous sanity cap on decoded pixel data, checked before allocation so a
// hostile header cannot request an absurd buffer.
constexpr uint64_t kMaxPixelBytes = 2ull << 30;  // 2 GiB (~23K x 23K x 4)

// Budget for all pre-composited animation frames together; an animation
// beyond it falls back to a static first frame.
constexpr uint64_t kMaxAnimationBytes = 1ull << 30;  // 1 GiB

// Clamp near-zero frame delays the way browsers do.
uint32_t NormalizeDelay(uint32_t delayMs)
{
    return delayMs < 20 ? 100 : delayMs;
}

bool TryReadUInt(IWICMetadataQueryReader* reader, PCWSTR name, uint32_t& value)
{
    if (reader == nullptr)
    {
        return false;
    }
    wil::unique_prop_variant var;
    if (FAILED(reader->GetMetadataByName(name, &var)))
    {
        return false;
    }
    switch (var.vt)
    {
    case VT_UI1:
        value = var.bVal;
        return true;
    case VT_UI2:
        value = var.uiVal;
        return true;
    case VT_UI4:
        value = var.ulVal;
        return true;
    default:
        return false;
    }
}

// Composites `source` (premultiplied BGRA, width x height) over `canvas` at
// (left, top). Alpha-over with premultiplied pixels is a simple add-blend.
void AlphaBlit(LoadedImage& canvas, const std::vector<uint8_t>& source, uint32_t sourceStride,
               uint32_t width, uint32_t height, uint32_t left, uint32_t top)
{
    const uint32_t right = std::min(canvas.width, left + width);
    const uint32_t bottom = std::min(canvas.height, top + height);
    for (uint32_t y = top; y < bottom; ++y)
    {
        const uint8_t* src = source.data() + size_t{y - top} * sourceStride;
        uint8_t* dst = canvas.pixels.data() + size_t{y} * canvas.stride + size_t{left} * 4;
        for (uint32_t x = left; x < right; ++x, src += 4, dst += 4)
        {
            const uint8_t alpha = src[3];
            if (alpha == 0xFF)
            {
                std::memcpy(dst, src, 4);
            }
            else if (alpha != 0)
            {
                const uint32_t inverse = 255u - alpha;
                dst[0] = static_cast<uint8_t>(src[0] + dst[0] * inverse / 255u);
                dst[1] = static_cast<uint8_t>(src[1] + dst[1] * inverse / 255u);
                dst[2] = static_cast<uint8_t>(src[2] + dst[2] * inverse / 255u);
                dst[3] = static_cast<uint8_t>(src[3] + dst[3] * inverse / 255u);
            }
        }
    }
}

void ClearRect(LoadedImage& canvas, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
    const uint32_t right = std::min(canvas.width, left + width);
    const uint32_t bottom = std::min(canvas.height, top + height);
    for (uint32_t y = top; y < bottom; ++y)
    {
        std::memset(canvas.pixels.data() + size_t{y} * canvas.stride + size_t{left} * 4, 0,
                    size_t{right - left} * 4);
    }
}

// Decodes every frame and composites it onto a persistent canvas, honoring
// the GIF frame rectangle and disposal method. Other containers (animated
// WebP via the OS codec) supply at least a frame delay; frames there are
// treated as full-canvas overlays, which matches how encoders commonly
// write them.
HRESULT DecodeAnimation(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
                        UINT frameCount, std::vector<AnimationFrame>& outFrames)
{
    // Canvas size: GIF logical screen if present, else the first frame.
    uint32_t canvasWidth = 0;
    uint32_t canvasHeight = 0;
    {
        wil::com_ptr<IWICMetadataQueryReader> containerReader;
        (void)decoder->GetMetadataQueryReader(containerReader.put());
        if (!TryReadUInt(containerReader.get(), L"/logscrdesc/Width", canvasWidth)
            || !TryReadUInt(containerReader.get(), L"/logscrdesc/Height", canvasHeight))
        {
            canvasWidth = 0;
            canvasHeight = 0;
        }
    }

    LoadedImage canvas;
    LoadedImage previousCanvas;  // for GIF disposal 3 (restore previous)
    std::vector<AnimationFrame> frames;

    for (UINT index = 0; index < frameCount; ++index)
    {
        wil::com_ptr<IWICBitmapFrameDecode> frame;
        RETURN_IF_FAILED(decoder->GetFrame(index, frame.put()));

        // Per-frame metadata; every read has a safe fallback.
        uint32_t left = 0;
        uint32_t top = 0;
        uint32_t delayMs = 100;
        uint32_t disposal = 0;
        {
            wil::com_ptr<IWICMetadataQueryReader> reader;
            (void)frame->GetMetadataQueryReader(reader.put());
            uint32_t value = 0;
            if (TryReadUInt(reader.get(), L"/grctlext/Delay", value))
            {
                delayMs = value * 10;  // GIF: 1/100s units
            }
            else if (TryReadUInt(reader.get(), L"/ANMF/FrameDuration", value))
            {
                delayMs = value;  // WebP: milliseconds
            }
            (void)TryReadUInt(reader.get(), L"/imgdesc/Left", left);
            (void)TryReadUInt(reader.get(), L"/imgdesc/Top", top);
            (void)TryReadUInt(reader.get(), L"/grctlext/Disposal", disposal);
        }

        wil::com_ptr<IWICFormatConverter> converter;
        RETURN_IF_FAILED(factory->CreateFormatConverter(converter.put()));
        RETURN_IF_FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom));
        UINT frameWidth = 0;
        UINT frameHeight = 0;
        RETURN_IF_FAILED(converter->GetSize(&frameWidth, &frameHeight));
        RETURN_HR_IF(WINCODEC_ERR_BADIMAGE, frameWidth == 0 || frameHeight == 0);

        if (canvas.pixels.empty())
        {
            if (canvasWidth == 0 || canvasHeight == 0)
            {
                canvasWidth = frameWidth;
                canvasHeight = frameHeight;
            }
            const uint64_t canvasBytes = uint64_t{canvasWidth} * canvasHeight * 4;
            RETURN_HR_IF(kHrImageTooLarge, canvasBytes > kMaxPixelBytes);
            RETURN_HR_IF(E_OUTOFMEMORY, canvasBytes * frameCount > kMaxAnimationBytes);
            canvas.width = canvasWidth;
            canvas.height = canvasHeight;
            canvas.stride = canvasWidth * 4;
            canvas.pixels.assign(size_t{canvas.stride} * canvasHeight, 0);
        }
        if (left >= canvas.width || top >= canvas.height)
        {
            left = 0;
            top = 0;
        }

        std::vector<uint8_t> pixels(size_t{frameWidth} * 4 * frameHeight);
        RETURN_IF_FAILED(converter->CopyPixels(nullptr, frameWidth * 4,
                                               static_cast<UINT>(pixels.size()),
                                               pixels.data()));

        if (disposal == 3)
        {
            previousCanvas = canvas;
        }
        AlphaBlit(canvas, pixels, frameWidth * 4, frameWidth, frameHeight, left, top);

        AnimationFrame composed;
        composed.delayMs = NormalizeDelay(delayMs);
        composed.image = canvas;  // snapshot
        frames.push_back(std::move(composed));

        switch (disposal)
        {
        case 2:  // restore background: clear the frame's area
            ClearRect(canvas, left, top, frameWidth, frameHeight);
            break;
        case 3:  // restore what was there before this frame
            if (!previousCanvas.pixels.empty())
            {
                canvas = previousCanvas;
            }
            break;
        default:
            break;  // 0/1: leave the frame in place
        }
    }

    outFrames = std::move(frames);
    return S_OK;
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
        // Read the file ourselves in chunks instead of letting WIC do the
        // I/O: WIC's file access has no cancellation point, while this loop
        // can bail out between chunks when a OneDrive hydration or SMB read
        // is slow. Known limit: a single ReadFile against a fully
        // unresponsive share can still block until the redirector times out
        // (CancelSynchronousIo could lift this later).
        wil::unique_hfile file(CreateFileW(request.path.c_str(), GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        RETURN_LAST_ERROR_IF(!file);

        LARGE_INTEGER fileSize{};
        RETURN_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &fileSize));
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE),
                     static_cast<uint64_t>(fileSize.QuadPart) > UINT_MAX);

        data.resize(static_cast<size_t>(fileSize.QuadPart));
        size_t offset = 0;
        while (offset < data.size())
        {
            if (ShouldAbort(stopToken))
            {
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            }
            const DWORD toRead =
                static_cast<DWORD>(std::min<size_t>(kReadChunkBytes, data.size() - offset));
            DWORD read = 0;
            RETURN_IF_WIN32_BOOL_FALSE(
                ReadFile(file.get(), data.data() + offset, toRead, &read, nullptr));
            RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF), read == 0);
            offset += read;
        }
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

    wil::com_ptr<IWICFormatConverter> converter;
    RETURN_IF_FAILED(factory->CreateFormatConverter(converter.put()));
    RETURN_IF_FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom));

    UINT width = 0;
    UINT height = 0;
    RETURN_IF_FAILED(converter->GetSize(&width, &height));
    RETURN_HR_IF(WINCODEC_ERR_BADIMAGE, width == 0 || height == 0);

    // 64-bit math: width/height come from the file and could overflow UINT.
    const uint64_t stride64 = uint64_t{width} * 4;
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
