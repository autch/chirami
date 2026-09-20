#pragma once

#include <wincodec.h>  // WIC container format GUIDs

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Which image formats chirami can write, and how file extensions map onto
// them. One table so the Save As filter list, the file type index the
// dialog reports back, and the format an extension selects cannot drift
// apart. Display names stay in the STRINGTABLE; only their ids live here.
//
// Nothing here includes framework.h, so the tests can link it.
struct SaveFormat
{
    const GUID* container;
    const wchar_t* filterSpec;        // dialog pattern, e.g. L"*.jpg;*.jpeg"
    const wchar_t* defaultExtension;  // no dot, e.g. L"jpg"
    unsigned filterNameId;            // STRINGTABLE id of the display name
};

// In the order the file dialog receives them; the dialog counts file types
// from 1, so index 0 here is file type 1 there.
std::span<const SaveFormat> SaveFormats();

// WIC container for a file extension (leading dot, any case), or GUID_NULL
// for anything chirami cannot encode.
GUID ContainerFormatFromExtension(std::wstring_view extension);

// 1-based file type index for a container format; 1 (PNG) when the format
// is not one of ours.
unsigned SaveFilterIndexFor(const GUID& container);

// The reverse, for the file type the dialog reports back. Out-of-range
// indices clamp into the table.
GUID ContainerFormatForFilterIndex(unsigned index);

// Splits the comma-separated extension list a WIC decoder reports
// (".jpeg,.jpg,.jfif") into lowercase entries. Empty entries are dropped.
std::vector<std::wstring> SplitExtensionList(std::wstring_view list);
