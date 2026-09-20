#include "ImageFormatId.h"

#include "PathCompare.h"
#include "resource.h"

#include <algorithm>

namespace
{

const SaveFormat kSaveFormats[] = {
    {&GUID_ContainerFormatPng, L"*.png", L"png", IDS_FILTER_PNG},
    {&GUID_ContainerFormatJpeg, L"*.jpg;*.jpeg", L"jpg", IDS_FILTER_JPEG},
    {&GUID_ContainerFormatBmp, L"*.bmp", L"bmp", IDS_FILTER_BMP},
    {&GUID_ContainerFormatTiff, L"*.tif;*.tiff", L"tif", IDS_FILTER_TIFF},
};

// Every extension that names one of the formats above, including the ones
// the dialog does not advertise (.jfif). A test keeps this in step with
// kSaveFormats by round-tripping each default extension.
struct ExtensionMapping
{
    const wchar_t* extension;
    const GUID* container;
};

const ExtensionMapping kExtensions[] = {
    {L".png", &GUID_ContainerFormatPng},   {L".jpg", &GUID_ContainerFormatJpeg},
    {L".jpeg", &GUID_ContainerFormatJpeg}, {L".jfif", &GUID_ContainerFormatJpeg},
    {L".bmp", &GUID_ContainerFormatBmp},   {L".tif", &GUID_ContainerFormatTiff},
    {L".tiff", &GUID_ContainerFormatTiff},
};

}  // namespace

std::span<const SaveFormat> SaveFormats()
{
    return kSaveFormats;
}

GUID ContainerFormatFromExtension(std::wstring_view extension)
{
    for (const ExtensionMapping& mapping : kExtensions)
    {
        if (EqualsNoCase(extension, mapping.extension))
        {
            return *mapping.container;
        }
    }
    return GUID_NULL;
}

unsigned SaveFilterIndexFor(const GUID& container)
{
    for (unsigned i = 0; i < std::size(kSaveFormats); ++i)
    {
        if (*kSaveFormats[i].container == container)
        {
            return i + 1;
        }
    }
    return 1;  // PNG
}

GUID ContainerFormatForFilterIndex(unsigned index)
{
    const unsigned clamped =
        std::clamp<unsigned>(index, 1, static_cast<unsigned>(std::size(kSaveFormats)));
    return *kSaveFormats[clamped - 1].container;
}

std::vector<std::wstring> SplitExtensionList(std::wstring_view list)
{
    std::vector<std::wstring> extensions;
    size_t start = 0;
    while (start < list.size())
    {
        const size_t comma = list.find(L',', start);
        const size_t end = (comma == std::wstring_view::npos) ? list.size() : comma;
        if (end > start)
        {
            extensions.push_back(ToLowerInvariant(list.substr(start, end - start)));
        }
        if (comma == std::wstring_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return extensions;
}
