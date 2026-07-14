#pragma once

#include "LoadedImage.h"

#include <filesystem>
#include <optional>
#include <vector>

// Tiny decoded-image cache for prefetching: it only ever holds the current
// file's neighbors (plus the image just navigated away from), so a plain
// vector suffices. UI thread only.
class ImageCache
{
public:
    // Total pixel-data budget. Two 8K images fit; anything bigger is not
    // worth caching and will just be decoded again on demand.
    static constexpr size_t kMaxTotalBytes = 512ull << 20;  // 512 MiB

    std::optional<LoadedImage> Take(const std::filesystem::path& path)
    {
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        {
            if (SamePath(it->path, path))
            {
                LoadedImage image = std::move(it->image);
                m_entries.erase(it);
                return image;
            }
        }
        return std::nullopt;
    }

    bool Contains(const std::filesystem::path& path) const
    {
        for (const auto& entry : m_entries)
        {
            if (SamePath(entry.path, path))
            {
                return true;
            }
        }
        return false;
    }

    void Put(std::filesystem::path path, LoadedImage image)
    {
        if (image.pixels.size() > kMaxTotalBytes)
        {
            return;
        }
        (void)Take(path);  // replace an existing entry for the same file
        m_entries.push_back({std::move(path), std::move(image)});
        while (TotalBytes() > kMaxTotalBytes && m_entries.size() > 1)
        {
            m_entries.erase(m_entries.begin());  // oldest first
        }
    }

    // Drops every entry whose path is not in `wanted`.
    void KeepOnly(const std::vector<std::filesystem::path>& wanted)
    {
        std::erase_if(m_entries, [&](const Entry& entry) {
            for (const auto& path : wanted)
            {
                if (SamePath(entry.path, path))
                {
                    return false;
                }
            }
            return true;
        });
    }

    void Clear() { m_entries.clear(); }

private:
    struct Entry
    {
        std::filesystem::path path;
        LoadedImage image;
    };

    static bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    size_t TotalBytes() const
    {
        size_t total = 0;
        for (const auto& entry : m_entries)
        {
            total += entry.image.pixels.size();
        }
        return total;
    }

    std::vector<Entry> m_entries;
};
