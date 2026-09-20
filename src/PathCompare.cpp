#include "PathCompare.h"

// Guarded: the test target defines both on the command line, the app gets
// them from framework.h, and this file is built into each.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <limits>

namespace
{

// The NLS APIs take lengths as int. A string that long cannot come from a
// path, but the conversion still has to be checked.
bool FitsInInt(size_t length)
{
    return length <= static_cast<size_t>((std::numeric_limits<int>::max)());
}

std::wstring MapCase(std::wstring_view text, DWORD flags)
{
    if (text.empty() || !FitsInInt(text.size()))
    {
        return std::wstring(text);
    }
    const int length = static_cast<int>(text.size());
    // LCMAP_LINGUISTIC_CASING is deliberately absent: casing stays ordinal,
    // so the result does not vary with the user's locale.
    const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, flags, text.data(), length, nullptr,
                                     0, nullptr, nullptr, 0);
    if (needed <= 0)
    {
        return std::wstring(text);
    }
    std::wstring mapped(static_cast<size_t>(needed), L'\0');
    const int written = LCMapStringEx(LOCALE_NAME_INVARIANT, flags, text.data(), length,
                                      mapped.data(), needed, nullptr, nullptr, 0);
    if (written <= 0)
    {
        return std::wstring(text);
    }
    mapped.resize(static_cast<size_t>(written));
    return mapped;
}

}  // namespace

bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
{
    if (a.empty() || b.empty())
    {
        // An empty view may hold a null pointer, which the API rejects.
        return a.empty() && b.empty();
    }
    if (!FitsInInt(a.size()) || !FitsInInt(b.size()))
    {
        return a == b;
    }
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool PathsEqualNoCase(const std::filesystem::path& a, const std::filesystem::path& b)
{
    return EqualsNoCase(a.native(), b.native());
}

std::wstring ToUpperInvariant(std::wstring_view text)
{
    return MapCase(text, LCMAP_UPPERCASE);
}

std::wstring ToLowerInvariant(std::wstring_view text)
{
    return MapCase(text, LCMAP_LOWERCASE);
}
