#pragma once

#include "framework.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// Enumerates decodable image files in a folder on a resident background
// thread, so directory listing (potentially slow on SMB shares) never blocks
// the UI. Same mailbox/notification contract as ImageLoader: the posted
// message is only a wake-up call, the result waits in TakeResult().
class FolderScanner
{
public:
    struct Result
    {
        uint64_t generation = 0;
        std::filesystem::path folder;
        std::vector<std::filesystem::path> files;  // full paths, sorted
    };

    FolderScanner(HWND notifyWindow, UINT notifyMessage);
    ~FolderScanner() = default;  // jthread stops and joins the worker

    FolderScanner(const FolderScanner&) = delete;
    FolderScanner& operator=(const FolderScanner&) = delete;

    // Queues a scan request, replacing any request not yet started (latest
    // wins). Returns the generation id to match against Result::generation.
    uint64_t RequestScan(std::filesystem::path folder);

    // Takes the most recent completed result, emptying the mailbox.
    std::optional<Result> TakeResult();

private:
    struct Request
    {
        uint64_t generation = 0;
        std::filesystem::path folder;
    };

    void WorkerProc(std::stop_token stopToken) noexcept;
    std::vector<std::filesystem::path> ScanFolder(const std::filesystem::path& folder,
                                                  const std::stop_token& stopToken);
    bool ShouldAbort(const std::stop_token& stopToken);

    HWND m_notifyWindow;
    UINT m_notifyMessage;

    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::optional<Request> m_pending;   // request slot (latest wins)
    std::optional<Result> m_completed;  // result mailbox
    uint64_t m_nextGeneration = 1;

    // Lowercase extensions of installed WIC decoders; built once on the
    // worker thread and only touched there.
    std::unordered_set<std::wstring> m_extensions;

    // Must remain the last member: destruction runs in reverse order, so the
    // worker is stopped and joined while the mutex/cv above are still alive.
    std::jthread m_thread;
};
