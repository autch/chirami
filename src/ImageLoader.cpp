#include "ImageLoader.h"

#include <shlwapi.h>  // SHCreateMemStream

#include <algorithm>
#include <utility>

namespace
{

constexpr DWORD kReadChunkBytes = 1u << 20;  // 1 MiB

// Generous sanity cap on decoded pixel data, checked before allocation so a
// hostile header cannot request an absurd buffer.
constexpr uint64_t kMaxPixelBytes = 2ull << 30;  // 2 GiB (~23K x 23K x 4)

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
        result.path = std::move(request.path);
        result.hr = DecodeFile(factory.get(), result.path, stopToken, result.image);

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

HRESULT ImageLoader::DecodeFile(IWICImagingFactory* factory, const std::filesystem::path& path,
                                const std::stop_token& stopToken, LoadedImage& out) noexcept
try
{
    // Read the file ourselves in chunks instead of letting WIC do the I/O:
    // WIC's file access has no cancellation point, while this loop can bail
    // out between chunks when a OneDrive hydration or SMB read is slow.
    // Known limit: a single ReadFile against a fully unresponsive share can
    // still block until the redirector times out (CancelSynchronousIo could
    // lift this later).
    wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    RETURN_LAST_ERROR_IF(!file);

    LARGE_INTEGER fileSize{};
    RETURN_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &fileSize));
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE),
                 static_cast<uint64_t>(fileSize.QuadPart) > UINT_MAX);

    std::vector<uint8_t> data(static_cast<size_t>(fileSize.QuadPart));
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
        RETURN_IF_WIN32_BOOL_FALSE(ReadFile(file.get(), data.data() + offset, toRead, &read, nullptr));
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF), read == 0);
        offset += read;
    }
    file.reset();

    wil::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(data.data(), static_cast<UINT>(data.size())));
    RETURN_HR_IF_NULL(E_OUTOFMEMORY, stream.get());
    data = {};  // SHCreateMemStream copied the bytes; drop ours

    wil::com_ptr<IWICBitmapDecoder> decoder;
    RETURN_IF_FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                      WICDecodeMetadataCacheOnDemand,
                                                      decoder.put()));

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
