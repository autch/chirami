#include "FolderScanner.h"

#include <shlwapi.h>  // StrCmpLogicalW

#include <algorithm>
#include <cwctype>
#include <utility>

namespace
{

std::wstring ToLower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text;
}

// Collects the file extensions of all installed WIC decoders, so optional
// codecs (WebP, AVIF, HEIF, ...) are picked up automatically.
std::unordered_set<std::wstring> QueryWicDecoderExtensions(IWICImagingFactory* factory)
{
    std::unordered_set<std::wstring> extensions;

    wil::com_ptr<IEnumUnknown> enumerator;
    THROW_IF_FAILED(factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault,
                                                       enumerator.put()));
    for (;;)
    {
        wil::com_ptr<IUnknown> unknown;
        ULONG fetched = 0;
        if (enumerator->Next(1, unknown.put(), &fetched) != S_OK)
        {
            break;
        }
        auto info = unknown.try_query<IWICBitmapDecoderInfo>();
        if (!info)
        {
            continue;
        }

        UINT length = 0;  // includes the terminating null
        if (FAILED(info->GetFileExtensions(0, nullptr, &length)) || length == 0)
        {
            continue;
        }
        std::wstring list(length, L'\0');
        if (FAILED(info->GetFileExtensions(length, list.data(), &length)))
        {
            continue;
        }
        list.resize(wcslen(list.c_str()));

        // Comma-separated, e.g. ".jpeg,.jpg,.jfif"
        size_t start = 0;
        while (start < list.size())
        {
            const size_t comma = list.find(L',', start);
            const size_t end = (comma == std::wstring::npos) ? list.size() : comma;
            if (end > start)
            {
                extensions.insert(ToLower(list.substr(start, end - start)));
            }
            if (comma == std::wstring::npos)
            {
                break;
            }
            start = comma + 1;
        }
    }
    return extensions;
}

}  // namespace

FolderScanner::FolderScanner(HWND notifyWindow, UINT notifyMessage)
    : m_notifyWindow(notifyWindow),
      m_notifyMessage(notifyMessage),
      m_thread([this](std::stop_token stopToken) { WorkerProc(stopToken); })
{
}

uint64_t FolderScanner::RequestScan(std::filesystem::path folder, SortSpec sort)
{
    uint64_t generation;
    {
        std::lock_guard lock(m_mutex);
        generation = m_nextGeneration++;
        m_pending = Request{generation, std::move(folder), sort};
    }
    m_cv.notify_one();
    return generation;
}

std::optional<FolderScanner::Result> FolderScanner::TakeResult()
{
    std::lock_guard lock(m_mutex);
    return std::exchange(m_completed, std::nullopt);
}

bool FolderScanner::ShouldAbort(const std::stop_token& stopToken)
{
    if (stopToken.stop_requested())
    {
        return true;
    }
    std::lock_guard lock(m_mutex);
    return m_pending.has_value();  // a newer request supersedes this one
}

void FolderScanner::WorkerProc(std::stop_token stopToken) noexcept
try
{
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    // Factory and extension set live below coInit so everything is released
    // before CoUninitialize; WIC objects never leave this thread.
    auto factory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);
    m_extensions = QueryWicDecoderExtensions(factory.get());

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
        result.folder = std::move(request.folder);
        result.files = ScanFolder(result.folder, request.sort, stopToken);

        if (stopToken.stop_requested())
        {
            return;
        }

        const uint64_t generation = result.generation;
        {
            std::lock_guard lock(m_mutex);
            m_completed = std::move(result);
        }
        PostMessageW(m_notifyWindow, m_notifyMessage, 0, static_cast<LPARAM>(generation));
    }
}
catch (...)
{
    // COM init or WIC enumeration failed; folder navigation stays inactive.
    LOG_CAUGHT_EXCEPTION();
}

std::vector<std::filesystem::path> FolderScanner::ScanFolder(const std::filesystem::path& folder,
                                                             const SortSpec& sort,
                                                             const std::stop_token& stopToken)
{
    namespace fs = std::filesystem;

    struct Entry
    {
        fs::path path;
        fs::file_time_type modified;
        uintmax_t size = 0;
    };

    std::vector<Entry> entries;
    std::error_code ec;
    for (auto it = fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        if (ShouldAbort(stopToken))
        {
            return {};
        }
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc))
        {
            continue;
        }
        if (!m_extensions.contains(ToLower(it->path().extension().wstring())))
        {
            continue;
        }
        // Skip hidden files, matching what Explorer shows by default.
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN))
        {
            continue;
        }
        Entry entry;
        entry.path = it->path();
        entry.modified = it->last_write_time(entryEc);
        entry.size = it->file_size(entryEc);
        entries.push_back(std::move(entry));
    }

    // Names compare in natural order, matching Explorer ("img2" before
    // "img10"); it is also the tie-breaker for the date and size keys.
    const auto nameLess = [](const Entry& a, const Entry& b) {
        return StrCmpLogicalW(a.path.filename().c_str(), b.path.filename().c_str()) < 0;
    };
    const auto ascendingLess = [&](const Entry& a, const Entry& b) {
        switch (sort.key)
        {
        case SortKey::Date:
            return a.modified != b.modified ? a.modified < b.modified : nameLess(a, b);
        case SortKey::Size:
            return a.size != b.size ? a.size < b.size : nameLess(a, b);
        case SortKey::Name:
        default:
            return nameLess(a, b);
        }
    };
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return sort.descending ? ascendingLess(b, a) : ascendingLess(a, b);
    });

    std::vector<fs::path> files;
    files.reserve(entries.size());
    for (auto& entry : entries)
    {
        files.push_back(std::move(entry.path));
    }
    return files;
}
