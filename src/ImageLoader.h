#pragma once

#include "framework.h"
#include "LoadedImage.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

// The decoded image exceeds the pixel budget or the render target's
// maximum bitmap size. Tiling (Phase 3) will lift this limit.
inline constexpr HRESULT kHrImageTooLarge = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0200);

// Decodes image files on a resident background thread so file I/O (which may
// stall on OneDrive hydration or slow SMB shares) never blocks the UI thread.
//
// Completion is signalled by posting `notifyMessage` to `notifyWindow`; the
// message carries no ownership — the result stays in an internal mailbox until
// the UI thread collects it with TakeResult(), so a lost message leaks nothing.
class ImageLoader
{
public:
    struct Result
    {
        uint64_t generation = 0;
        std::filesystem::path path;
        HRESULT hr = E_FAIL;
        LoadedImage image;  // valid only when SUCCEEDED(hr)
    };

    ImageLoader(HWND notifyWindow, UINT notifyMessage);
    ~ImageLoader() = default;  // jthread stops and joins the worker

    ImageLoader(const ImageLoader&) = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // Queues a load request, replacing any request not yet started (latest
    // wins). Returns the generation id to match against Result::generation.
    uint64_t RequestLoad(std::filesystem::path path);

    // Decodes an in-memory blob (e.g. clipboard data) instead of a file.
    // displayName is reported back as Result::path for the title bar.
    uint64_t RequestLoadFromMemory(std::vector<uint8_t> data, std::filesystem::path displayName);

    // Takes the most recent completed result, emptying the mailbox.
    std::optional<Result> TakeResult();

private:
    struct Request
    {
        uint64_t generation = 0;
        std::filesystem::path path;
        std::vector<uint8_t> data;  // non-empty: decode this instead of path
    };

    void WorkerProc(std::stop_token stopToken) noexcept;
    HRESULT Decode(IWICImagingFactory* factory, Request& request,
                   const std::stop_token& stopToken, LoadedImage& out) noexcept;
    bool ShouldAbort(const std::stop_token& stopToken);

    HWND m_notifyWindow;
    UINT m_notifyMessage;

    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::optional<Request> m_pending;    // request slot (latest wins)
    std::optional<Result> m_completed;   // result mailbox
    uint64_t m_nextGeneration = 1;

    // Must remain the last member: destruction runs in reverse order, so the
    // worker is stopped and joined while the mutex/cv above are still alive.
    std::jthread m_thread;
};
