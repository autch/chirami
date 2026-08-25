#pragma once

#include "framework.h"
#include "ImageMetadata.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Collects file and image metadata for the properties window on a resident
// background thread, mirroring ImageLoader's mailbox pattern: completion is
// signalled by posting `notifyMessage` to `notifyWindow`; the result stays
// in an internal mailbox until the UI thread collects it with TakeResult().
class MetadataReader
{
public:
    struct Result
    {
        uint64_t generation = 0;
        std::filesystem::path path;
        HRESULT hr = E_FAIL;               // overall failure; items may still
                                           // be partial when metadata is odd
        std::vector<MetadataItem> items;
    };

    // `language` is applied to the worker thread so string resources load
    // in the same language as the UI.
    MetadataReader(HWND notifyWindow, UINT notifyMessage, LANGID language);
    ~MetadataReader() = default;  // jthread stops and joins the worker

    MetadataReader(const MetadataReader&) = delete;
    MetadataReader& operator=(const MetadataReader&) = delete;

    // Queues a read request, replacing any request not yet started (latest
    // wins). Returns the generation id to match against Result::generation.
    uint64_t RequestRead(std::filesystem::path path);

    // Takes the most recent completed result, emptying the mailbox.
    std::optional<Result> TakeResult();

private:
    struct Request
    {
        uint64_t generation = 0;
        std::filesystem::path path;
    };

    void WorkerProc(std::stop_token stopToken) noexcept;
    HRESULT Read(IWICImagingFactory* factory, const Request& request,
                 const std::stop_token& stopToken, std::vector<MetadataItem>& items) noexcept;
    bool ShouldAbort(const std::stop_token& stopToken);

    HWND m_notifyWindow;
    UINT m_notifyMessage;
    LANGID m_language;

    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::optional<Request> m_pending;    // request slot (latest wins)
    std::optional<Result> m_completed;   // result mailbox
    uint64_t m_nextGeneration = 1;

    // Must remain the last member: destruction runs in reverse order, so the
    // worker is stopped and joined while the mutex/cv above are still alive.
    std::jthread m_thread;
};
