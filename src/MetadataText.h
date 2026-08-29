#pragma once

#include "framework.h"

#include <algorithm>
#include <string>

// Sanity caps shared by PNG text parsing and WIC metadata enumeration: a
// hostile file must not turn the properties window into a memory hog.
inline constexpr size_t kMaxValueChars = 256 * 1024;
inline constexpr size_t kMaxWicItems = 512;

// UTF-8 first, then code page 1252. PNG tEXt is Latin-1 by spec, but AI
// tools (ComfyUI and friends) routinely write UTF-8 into it; EXIF ASCII
// fields get the same treatment. 1252 accepts any byte sequence, so this
// never fails.
inline std::wstring WidenBytes(const char* bytes, size_t length)
{
    if (length == 0 || bytes == nullptr)
    {
        return {};
    }
    length = std::min(length, kMaxValueChars * 3);
    const int inLength = static_cast<int>(length);
    UINT codePage = CP_UTF8;
    int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, inLength, nullptr, 0);
    if (wideLength <= 0)
    {
        codePage = 1252;
        wideLength = MultiByteToWideChar(codePage, 0, bytes, inLength, nullptr, 0);
        if (wideLength <= 0)
        {
            return {};
        }
    }
    std::wstring out(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(codePage, codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes, inLength,
                        out.data(), wideLength);
    while (!out.empty() && out.back() == L'\0')
    {
        out.pop_back();
    }
    return out;
}

inline std::wstring ClampValue(std::wstring value)
{
    if (value.size() > kMaxValueChars)
    {
        value.resize(kMaxValueChars);
        value += L" …";
    }
    return value;
}
