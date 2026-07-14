#pragma once

#include "framework.h"
#include "LoadedImage.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

// Encodes and writes images on a resident background thread so a save to a
// slow destination (SMB, OneDrive) never blocks the UI. Same
// mailbox/notification contract as ImageLoader.
class ImageSaver
{
public:
    struct Result
    {
        std::filesystem::path path;
        HRESULT hr = E_FAIL;
    };

    ImageSaver(HWND notifyWindow, UINT notifyMessage);
    ~ImageSaver() = default;  // jthread stops and joins the worker

    ImageSaver(const ImageSaver&) = delete;
    ImageSaver& operator=(const ImageSaver&) = delete;

    // Queues a save. Returns false (and does nothing) while a save is still
    // pending; saves are rare enough that a one-deep queue suffices.
    bool RequestSave(std::filesystem::path path, GUID containerFormat, LoadedImage image);

    // Takes the most recent completed result, emptying the mailbox.
    std::optional<Result> TakeResult();

private:
    struct Request
    {
        std::filesystem::path path;
        GUID containerFormat{};
        LoadedImage image;
    };

    void WorkerProc(std::stop_token stopToken) noexcept;
    static HRESULT SaveImage(IWICImagingFactory* factory, const Request& request) noexcept;

    HWND m_notifyWindow;
    UINT m_notifyMessage;

    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::optional<Request> m_pending;
    std::optional<Result> m_completed;

    // Must remain the last member: destruction runs in reverse order, so the
    // worker is stopped and joined while the mutex/cv above are still alive.
    std::jthread m_thread;
};
