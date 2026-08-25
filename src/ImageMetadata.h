#pragma once

#include <cstdint>
#include <string>

// One row of the properties window. `name` and `value` are display-ready
// (localized by the reader's worker thread where applicable); group titles
// are resolved from the string table on the UI thread.
enum class MetadataGroup : uint8_t
{
    File,     // path, size, timestamps
    Image,    // container format, dimensions, pixel format, DPI, frames
    PngText,  // PNG tEXt/iTXt/zTXt chunks (AI generation prompts live here)
    Wic,      // everything else the WIC metadata query reader exposes
};

struct MetadataItem
{
    MetadataGroup group = MetadataGroup::File;
    std::wstring name;
    std::wstring value;
};
