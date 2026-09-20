#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Ordinal (non-linguistic) comparison and case folding for file names,
// paths and extensions. The CRT equivalents (_wcsicmp, towlower) fold by the
// current locale and disagree with the file system on anything outside
// ASCII; see DESIGN.md, "String comparison and path identity".
//
// Nothing here includes framework.h, so the tests can link it.

// Case-insensitive equality by the OS uppercase table - the rule NTFS
// itself uses. Two empty strings are equal.
bool EqualsNoCase(std::wstring_view a, std::wstring_view b);

// Same comparison over whole paths. Purely textual: short (8.3) names,
// junctions and \?\ prefixes still compare unequal, which callers accept
// (see DESIGN.md for where that is and is not good enough).
bool PathsEqualNoCase(const std::filesystem::path& a, const std::filesystem::path& b);

// Invariant-locale case folding, for when a normalized string is needed
// rather than a comparison (a set key, a registry name). Uppercase is the
// direction the file system folds in and the one that loses no information;
// the lowercase variant is for strings that are written out in lowercase by
// contract. Both return the input unchanged if the OS call fails.
std::wstring ToUpperInvariant(std::wstring_view text);
std::wstring ToLowerInvariant(std::wstring_view text);
