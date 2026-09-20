#include "ImageFormatId.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("Extensions select a format regardless of case", "[imageformat]")
{
    REQUIRE(ContainerFormatFromExtension(L".png") == GUID_ContainerFormatPng);
    REQUIRE(ContainerFormatFromExtension(L".JPG") == GUID_ContainerFormatJpeg);
    REQUIRE(ContainerFormatFromExtension(L".JpEg") == GUID_ContainerFormatJpeg);
    REQUIRE(ContainerFormatFromExtension(L".jfif") == GUID_ContainerFormatJpeg);
    REQUIRE(ContainerFormatFromExtension(L".TIFF") == GUID_ContainerFormatTiff);
}

TEST_CASE("Formats chirami cannot encode report no container", "[imageformat]")
{
    // Decodable, but there is no encoder: the save dialog offers PNG instead.
    REQUIRE(ContainerFormatFromExtension(L".gif") == GUID_NULL);
    REQUIRE(ContainerFormatFromExtension(L".webp") == GUID_NULL);
    REQUIRE(ContainerFormatFromExtension(L"") == GUID_NULL);
    REQUIRE(ContainerFormatFromExtension(L"png") == GUID_NULL);  // the dot is required
    REQUIRE(ContainerFormatFromExtension(L"写真.jpg") == GUID_NULL);  // an extension, not a name
}

TEST_CASE("File type indices round-trip through the table", "[imageformat]")
{
    unsigned index = 1;
    for (const SaveFormat& format : SaveFormats())
    {
        REQUIRE(SaveFilterIndexFor(*format.container) == index);
        REQUIRE(ContainerFormatForFilterIndex(index) == *format.container);
        ++index;
    }
}

TEST_CASE("Out-of-range file type indices clamp instead of reading past the table",
          "[imageformat]")
{
    const GUID first = *SaveFormats().front().container;
    const GUID last = *SaveFormats().back().container;
    REQUIRE(ContainerFormatForFilterIndex(0) == first);
    REQUIRE(ContainerFormatForFilterIndex(99) == last);
    REQUIRE(SaveFilterIndexFor(GUID_NULL) == 1);  // unknown format defaults to PNG
}

TEST_CASE("Every save format's default extension names that same format", "[imageformat]")
{
    // What keeps the dialog's filter list and the extension lookup in step.
    for (const SaveFormat& format : SaveFormats())
    {
        const std::wstring extension = L"." + std::wstring(format.defaultExtension);
        REQUIRE(ContainerFormatFromExtension(extension) == *format.container);
        REQUIRE(format.filterNameId != 0);
        REQUIRE(format.defaultExtension[0] != L'.');  // the dialog wants it bare
    }
}

TEST_CASE("A WIC extension list splits into lowercase entries", "[imageformat]")
{
    const std::vector<std::wstring> jpeg = SplitExtensionList(L".jpeg,.jpg,.jfif");
    REQUIRE(jpeg == std::vector<std::wstring>{L".jpeg", L".jpg", L".jfif"});

    REQUIRE(SplitExtensionList(L".PNG") == std::vector<std::wstring>{L".png"});
    REQUIRE(SplitExtensionList(L"") == std::vector<std::wstring>{});
    // Stray separators are dropped rather than producing empty extensions.
    REQUIRE(SplitExtensionList(L",.bmp,,") == std::vector<std::wstring>{L".bmp"});
}
