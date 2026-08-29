#pragma once

#include "framework.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

// STRINGTABLE lookup. The thread UI language (SetThreadUILanguage) selects
// ja or en. Buffer is large enough for every current resource string.
inline std::wstring LoadStringResource(UINT id)
{
    WCHAR buffer[512];
    const int length = LoadStringW(_Module.GetResourceInstance(), id, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, length > 0 ? static_cast<size_t>(length) : 0);
}

inline std::wstring ToLower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text;
}

inline std::wstring ToUpper(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    return text;
}

// NTFS and Explorer treat file names as case-insensitive.
inline bool PathsEqualNoCase(const std::filesystem::path& a, const std::filesystem::path& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}
