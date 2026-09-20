#pragma once

#include "framework.h"

#include <string>

// STRINGTABLE lookup. The thread UI language (SetThreadUILanguage) selects
// ja or en. Buffer is large enough for every current resource string.
inline std::wstring LoadStringResource(UINT id)
{
    WCHAR buffer[512];
    const int length = LoadStringW(_Module.GetResourceInstance(), id, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, length > 0 ? static_cast<size_t>(length) : 0);
}
