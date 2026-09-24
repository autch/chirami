#include "ExifOrientation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct Entry
{
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t value;  // left-justified SHORTs are handled by the writer
};

class TiffWriter
{
public:
    explicit TiffWriter(bool bigEndian) : m_bigEndian(bigEndian) {}

    void U16(uint16_t value)
    {
        if (m_bigEndian)
        {
            m_bytes.push_back(static_cast<uint8_t>(value >> 8));
            m_bytes.push_back(static_cast<uint8_t>(value));
        }
        else
        {
            m_bytes.push_back(static_cast<uint8_t>(value));
            m_bytes.push_back(static_cast<uint8_t>(value >> 8));
        }
    }

    void U32(uint32_t value)
    {
        if (m_bigEndian)
        {
            U16(static_cast<uint16_t>(value >> 16));
            U16(static_cast<uint16_t>(value));
        }
        else
        {
            U16(static_cast<uint16_t>(value));
            U16(static_cast<uint16_t>(value >> 16));
        }
    }

    std::vector<uint8_t>& Bytes() { return m_bytes; }

private:
    bool m_bigEndian;
    std::vector<uint8_t> m_bytes;
};

// A TIFF header with one IFD holding `entries`.
std::vector<uint8_t> MakeTiff(bool bigEndian, const std::vector<Entry>& entries)
{
    TiffWriter writer(bigEndian);
    writer.Bytes().push_back(bigEndian ? 'M' : 'I');
    writer.Bytes().push_back(bigEndian ? 'M' : 'I');
    writer.U16(42);
    writer.U32(8);
    writer.U16(static_cast<uint16_t>(entries.size()));
    for (const Entry& entry : entries)
    {
        writer.U16(entry.tag);
        writer.U16(entry.type);
        writer.U32(entry.count);
        if (entry.type == 3 && entry.count == 1)
        {
            writer.U16(static_cast<uint16_t>(entry.value));
            writer.U16(0);
        }
        else
        {
            writer.U32(entry.value);
        }
    }
    writer.U32(0);  // no next IFD
    return std::move(writer.Bytes());
}

void AppendSegment(std::vector<uint8_t>& jpeg, uint8_t marker, const std::vector<uint8_t>& payload)
{
    jpeg.push_back(0xFF);
    jpeg.push_back(marker);
    const size_t length = payload.size() + 2;
    jpeg.push_back(static_cast<uint8_t>(length >> 8));
    jpeg.push_back(static_cast<uint8_t>(length));
    jpeg.insert(jpeg.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> ExifPayload(const std::vector<uint8_t>& tiff)
{
    std::vector<uint8_t> payload = {'E', 'x', 'i', 'f', 0, 0};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    return payload;
}

// SOI, an APP0, the Exif APP1, then the start of scan.
std::vector<uint8_t> MakeJpeg(const std::vector<uint8_t>& tiff)
{
    std::vector<uint8_t> jpeg = {0xFF, 0xD8};
    AppendSegment(jpeg, 0xE0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    AppendSegment(jpeg, 0xE1, ExifPayload(tiff));
    AppendSegment(jpeg, 0xDA, {0, 0});
    return jpeg;
}

std::vector<uint8_t> JpegWithOrientation(uint16_t orientation, bool bigEndian)
{
    // Make (ASCII, offset) before the Orientation, as cameras write them.
    return MakeJpeg(MakeTiff(bigEndian, {{271, 2, 6, 0}, {274, 3, 1, orientation}}));
}

// Where a stored pixel lands on screen after applying `steps`.
struct Point
{
    uint32_t x;
    uint32_t y;
    bool operator==(const Point&) const = default;
};

Point Apply(const OrientationSteps& steps, Point p, uint32_t width, uint32_t height)
{
    if (steps.flipHorizontal)
    {
        p.x = width - 1 - p.x;
    }
    if (steps.flipVertical)
    {
        p.y = height - 1 - p.y;
    }
    for (int i = 0; i < steps.quarterTurnsClockwise; ++i)
    {
        p = {height - 1 - p.y, p.x};
        std::swap(width, height);
    }
    return p;
}

}  // namespace

TEST_CASE("Orientation is read in either byte order", "[exif]")
{
    const bool bigEndian = GENERATE(false, true);
    const uint16_t orientation = GENERATE(1, 2, 3, 4, 5, 6, 7, 8);
    REQUIRE(ReadJpegExifOrientation(JpegWithOrientation(orientation, bigEndian)) == orientation);
}

TEST_CASE("Missing orientation reads as normal", "[exif]")
{
    REQUIRE(ReadJpegExifOrientation(MakeJpeg(MakeTiff(true, {{271, 2, 6, 0}}))) == 1);

    // No Exif segment at all.
    std::vector<uint8_t> jpeg = {0xFF, 0xD8};
    AppendSegment(jpeg, 0xE0, {'J', 'F', 'I', 'F', 0});
    AppendSegment(jpeg, 0xDA, {0, 0});
    REQUIRE(ReadJpegExifOrientation(jpeg) == 1);
}

TEST_CASE("An Exif segment after the scan is ignored", "[exif]")
{
    std::vector<uint8_t> jpeg = {0xFF, 0xD8};
    AppendSegment(jpeg, 0xDA, {0, 0});
    AppendSegment(jpeg, 0xE1, ExifPayload(MakeTiff(false, {{274, 3, 1, 6}})));
    REQUIRE(ReadJpegExifOrientation(jpeg) == 1);
}

TEST_CASE("Malformed orientation data reads as normal", "[exif]")
{
    SECTION("not a JPEG")
    {
        REQUIRE(ReadJpegExifOrientation({}) == 1);
        const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        REQUIRE(ReadJpegExifOrientation(png) == 1);
    }
    SECTION("value out of range")
    {
        REQUIRE(ReadJpegExifOrientation(JpegWithOrientation(9, false)) == 1);
        REQUIRE(ReadJpegExifOrientation(JpegWithOrientation(0, true)) == 1);
    }
    SECTION("wrong type")
    {
        REQUIRE(ReadJpegExifOrientation(MakeJpeg(MakeTiff(false, {{274, 4, 1, 6}}))) == 1);
    }
    SECTION("bad byte order mark")
    {
        auto tiff = MakeTiff(false, {{274, 3, 1, 6}});
        tiff[0] = 'X';
        REQUIRE(ReadJpegExifOrientation(MakeJpeg(tiff)) == 1);
    }
    SECTION("IFD offset past the end")
    {
        auto tiff = MakeTiff(false, {{274, 3, 1, 6}});
        tiff[4] = 0xF0;
        REQUIRE(ReadJpegExifOrientation(MakeJpeg(tiff)) == 1);
    }
    SECTION("segment length past the end")
    {
        auto jpeg = JpegWithOrientation(6, false);
        jpeg.resize(jpeg.size() - 20);
        REQUIRE(ReadJpegExifOrientation(jpeg) == 1);
    }
    SECTION("entry count past the end")
    {
        auto tiff = MakeTiff(false, {{274, 3, 1, 6}});
        tiff[8] = 0xFF;  // low byte of the entry count
        tiff[9] = 0x00;
        // The one real entry still precedes the bogus ones.
        REQUIRE(ReadJpegExifOrientation(MakeJpeg(tiff)) == 6);
        tiff = MakeTiff(false, {{271, 2, 6, 0}});
        tiff[8] = 0xFF;
        REQUIRE(ReadJpegExifOrientation(MakeJpeg(tiff)) == 1);
    }
}

TEST_CASE("Steps put the stored corners where EXIF says", "[exif]")
{
    // EXIF defines each value by which screen edge the stored 0th row and
    // 0th column belong to; the stored top-left and top-right pixels then
    // land on these screen corners.
    enum Corner
    {
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };
    struct Expected
    {
        uint16_t orientation;
        Corner topLeft;
        Corner topRight;
        bool swapsAxes;
    };
    const Expected table[] = {
        {1, TopLeft, TopRight, false},     {2, TopRight, TopLeft, false},
        {3, BottomRight, BottomLeft, false}, {4, BottomLeft, BottomRight, false},
        {5, TopLeft, BottomLeft, true},    {6, TopRight, BottomRight, true},
        {7, BottomRight, TopRight, true},  {8, BottomLeft, TopLeft, true},
    };

    constexpr uint32_t width = 4;
    constexpr uint32_t height = 3;
    for (const Expected& expected : table)
    {
        INFO("orientation " << expected.orientation);
        const OrientationSteps steps = StepsForOrientation(expected.orientation);
        const uint32_t shownWidth = expected.swapsAxes ? height : width;
        const uint32_t shownHeight = expected.swapsAxes ? width : height;
        const auto corner = [&](Corner c) {
            return Point{(c == TopRight || c == BottomRight) ? shownWidth - 1 : 0,
                         (c == BottomLeft || c == BottomRight) ? shownHeight - 1 : 0};
        };
        REQUIRE(Apply(steps, {0, 0}, width, height) == corner(expected.topLeft));
        REQUIRE(Apply(steps, {width - 1, 0}, width, height) == corner(expected.topRight));
        REQUIRE((steps.quarterTurnsClockwise % 2 == 1) == expected.swapsAxes);
    }
}

TEST_CASE("Normal and unknown orientations need no work", "[exif]")
{
    REQUIRE(StepsForOrientation(1).IsIdentity());
    REQUIRE(StepsForOrientation(0).IsIdentity());
    REQUIRE(StepsForOrientation(9).IsIdentity());
    REQUIRE_FALSE(StepsForOrientation(6).IsIdentity());
}
