#include "PathCompare.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("Names differing only in ASCII case are equal", "[pathcompare]")
{
    REQUIRE(EqualsNoCase(L"IMG_001.JPG", L"img_001.jpg"));
    REQUIRE(EqualsNoCase(L".TIFF", L".tiff"));
    REQUIRE_FALSE(EqualsNoCase(L"img_001.jpg", L"img_002.jpg"));
}

TEST_CASE("Kanji and spaces survive the comparison", "[pathcompare]")
{
    REQUIRE(EqualsNoCase(L"写真 001.JPG", L"写真 001.jpg"));
    REQUIRE_FALSE(EqualsNoCase(L"写真 001.jpg", L"写真001.jpg"));  // the space matters
    REQUIRE_FALSE(EqualsNoCase(L"写真.jpg", L"寫真.jpg"));
    // Kanji have no case, so folding must leave them alone.
    REQUIRE(ToUpperInvariant(L"写真 001") == L"写真 001");
}

TEST_CASE("Full-width letters fold like the file system, not like the CRT", "[pathcompare]")
{
    // _wcsicmp with the default locale would report these as different.
    REQUIRE(EqualsNoCase(L"画像Ａ.JPG", L"画像ａ.jpg"));
    REQUIRE(ToUpperInvariant(L"ａｂｃ") == L"ＡＢＣ");
}

TEST_CASE("Empty strings compare as equal to each other only", "[pathcompare]")
{
    REQUIRE(EqualsNoCase(L"", L""));
    REQUIRE_FALSE(EqualsNoCase(L"", L".jpg"));
    REQUIRE(ToUpperInvariant(L"") == L"");
    REQUIRE(ToLowerInvariant(L"") == L"");
}

TEST_CASE("Trailing dots and spaces are significant", "[pathcompare]")
{
    // Win32 strips these when opening a file, but as strings they differ;
    // callers must not rely on textual comparison to answer "same file".
    REQUIRE_FALSE(EqualsNoCase(L"photo.jpg ", L"photo.jpg"));
    REQUIRE_FALSE(EqualsNoCase(L"photo.jpg.", L"photo.jpg"));
}

TEST_CASE("Whole paths compare case-insensitively", "[pathcompare]")
{
    REQUIRE(PathsEqualNoCase(LR"(C:\写真 2026\IMG_001.JPG)", LR"(c:\写真 2026\img_001.jpg)"));
    REQUIRE(PathsEqualNoCase(LR"(\server\共有\a.png)", LR"(\SERVER\共有\A.PNG)"));
    REQUIRE_FALSE(PathsEqualNoCase(LR"(C:\a\b.png)", LR"(C:\a\c.png)"));
}

TEST_CASE("Textual path comparison cannot see through prefixes", "[pathcompare]")
{
    // Documented limitation, not a defect: the extended-length form names
    // the same file but is a different string. Callers that need certainty
    // have to go through the file system instead.
    REQUIRE_FALSE(PathsEqualNoCase(LR"(\?\C:\写真\a.jpg)", LR"(C:\写真\a.jpg)"));
    REQUIRE_FALSE(PathsEqualNoCase(LR"(C:\写真\a.jpg)", LR"(C:\写真\.\a.jpg)"));
}

TEST_CASE("Case folding is reversible for ASCII extensions", "[pathcompare]")
{
    REQUIRE(ToUpperInvariant(L".jpeg") == L".JPEG");
    REQUIRE(ToLowerInvariant(L".JPEG") == L".jpeg");
    // The ProgID suffix the registry holds is built this way.
    REQUIRE(ToUpperInvariant(std::wstring_view(L".webp").substr(1)) == L"WEBP");
}
