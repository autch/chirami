#pragma once

#include "framework.h"
#include "resource.h"

#include <string>

// STRINGTABLE lookup. The thread UI language (SetThreadUILanguage) selects
// ja or en. Buffer is large enough for every current resource string.
inline std::wstring LoadStringResource(UINT id)
{
    WCHAR buffer[512];
    const int length = LoadStringW(_Module.GetResourceInstance(), id, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, length > 0 ? static_cast<size_t>(length) : 0);
}

// Empty in a release build. Debug builds carry it wherever the app names
// itself, so a debug binary is never mistaken for the real one.
inline std::wstring DebugMark()
{
#ifdef _DEBUG
    return LoadStringResource(IDS_DEBUG_MARK);
#else
    return {};
#endif
}

// The application name as the title bar shows it.
inline std::wstring AppTitle()
{
    return LoadStringResource(IDS_APP_TITLE) + DebugMark();
}
