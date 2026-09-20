#include "ProgIds.h"

#include <catch2/catch_test_macros.hpp>

// These strings sit in the registry of every user who registered the file
// associations. Changing them orphans those entries, so the exact spelling
// is pinned here on purpose.
TEST_CASE("ProgIDs keep their registered spelling", "[progid]")
{
    REQUIRE(ProgIdForExtension(L".jpg") == L"chirami.AssocFile.JPG");
    REQUIRE(ProgIdForExtension(L".png") == L"chirami.AssocFile.PNG");
    REQUIRE(ProgIdForExtension(L".jfif") == L"chirami.AssocFile.JFIF");
    REQUIRE(ProgIdForExtension(L".webp") == L"chirami.AssocFile.WEBP");
}

TEST_CASE("The extension label drops the dot and uppercases", "[progid]")
{
    REQUIRE(ExtensionLabel(L".tiff") == L"TIFF");
    REQUIRE(ExtensionLabel(L".HEIC") == L"HEIC");
    REQUIRE(ExtensionLabel(L"avif") == L"AVIF");  // tolerates a missing dot
    REQUIRE(ExtensionLabel(L"") == L"");
    REQUIRE(ExtensionLabel(L".") == L"");
}

TEST_CASE("Only chirami's own class names are recognized", "[progid]")
{
    REQUIRE(IsChiramiProgId(L"chirami.AssocFile.JPG"));
    REQUIRE(IsChiramiProgId(L"CHIRAMI.ASSOCFILE.JPG"));  // registry keys fold case
    REQUIRE_FALSE(IsChiramiProgId(L"chirami.Assoc"));    // shorter than the prefix
    REQUIRE_FALSE(IsChiramiProgId(L"PhotoViewer.FileAssoc.Jpeg"));
    REQUIRE_FALSE(IsChiramiProgId(L""));
}
