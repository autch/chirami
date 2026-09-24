#include "IccProfile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{

void PutU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void PutS15Fixed16(std::vector<uint8_t>& out, double value)
{
    PutU32(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(value * 65536.0))));
}

void PutSignature(std::vector<uint8_t>& out, const char* signature)
{
    out.insert(out.end(), signature, signature + 4);
}

std::vector<uint8_t> XyzTag(double x, double y, double z)
{
    std::vector<uint8_t> tag;
    PutSignature(tag, "XYZ ");
    PutU32(tag, 0);
    PutS15Fixed16(tag, x);
    PutS15Fixed16(tag, y);
    PutS15Fixed16(tag, z);
    return tag;
}

std::vector<uint8_t> ParaTag(const std::vector<double>& params, uint16_t functionType = 3)
{
    std::vector<uint8_t> tag;
    PutSignature(tag, "para");
    PutU32(tag, 0);
    tag.push_back(static_cast<uint8_t>(functionType >> 8));
    tag.push_back(static_cast<uint8_t>(functionType));
    tag.push_back(0);
    tag.push_back(0);
    for (double param : params)
    {
        PutS15Fixed16(tag, param);
    }
    return tag;
}

const std::vector<double> kSrgbCurve = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};

using TagList = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

// A sampled curveType tag of `count` entries from `curve` over 0..1.
template <typename Curve>
std::vector<uint8_t> CurvTag(size_t count, Curve curve)
{
    std::vector<uint8_t> tag;
    PutSignature(tag, "curv");
    PutU32(tag, 0);
    PutU32(tag, static_cast<uint32_t>(count));
    for (size_t i = 0; i < count; ++i)
    {
        const double value = curve(static_cast<double>(i) / static_cast<double>(count - 1));
        const auto sample = static_cast<uint16_t>(std::lround(value * 65535.0));
        tag.push_back(static_cast<uint8_t>(sample >> 8));
        tag.push_back(static_cast<uint8_t>(sample));
    }
    return tag;
}

double SrgbToLinear(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// A matrix/TRC display profile with the given D50-adapted colorants and
// the same tone curve tag on all three channels.
std::vector<uint8_t> MakeProfileWithTrc(const double (&red)[3], const double (&green)[3],
                                        const double (&blue)[3], const std::vector<uint8_t>& trc,
                                        const TagList& extraTags = {})
{
    TagList tags = {
        {"rXYZ", XyzTag(red[0], red[1], red[2])},
        {"gXYZ", XyzTag(green[0], green[1], green[2])},
        {"bXYZ", XyzTag(blue[0], blue[1], blue[2])},
        {"rTRC", trc},
        {"gTRC", trc},
        {"bTRC", trc},
    };
    tags.insert(tags.end(), extraTags.begin(), extraTags.end());

    std::vector<uint8_t> profile(128, 0);
    std::memcpy(profile.data() + 12, "mntr", 4);
    std::memcpy(profile.data() + 16, "RGB ", 4);
    std::memcpy(profile.data() + 20, "XYZ ", 4);
    std::memcpy(profile.data() + 36, "acsp", 4);
    PutU32(profile, static_cast<uint32_t>(tags.size()));

    size_t offset = profile.size() + tags.size() * 12;
    std::vector<uint8_t> data;
    for (const auto& [signature, tag] : tags)
    {
        PutSignature(profile, signature.c_str());
        PutU32(profile, static_cast<uint32_t>(offset + data.size()));
        PutU32(profile, static_cast<uint32_t>(tag.size()));
        data.insert(data.end(), tag.begin(), tag.end());
    }
    profile.insert(profile.end(), data.begin(), data.end());
    return profile;
}

// The same with a parametric curve.
std::vector<uint8_t> MakeProfile(const double (&red)[3], const double (&green)[3],
                                 const double (&blue)[3],
                                 const std::vector<double>& curve = kSrgbCurve,
                                 const TagList& extraTags = {})
{
    return MakeProfileWithTrc(red, green, blue, ParaTag(curve), extraTags);
}

// v2 textDescriptionType.
std::vector<uint8_t> DescTag(const std::string& text)
{
    std::vector<uint8_t> tag;
    PutSignature(tag, "desc");
    PutU32(tag, 0);
    PutU32(tag, static_cast<uint32_t>(text.size() + 1));
    tag.insert(tag.end(), text.begin(), text.end());
    tag.push_back(0);
    tag.resize(tag.size() + 79, 0);  // the (unused) Unicode and ScriptCode parts
    return tag;
}

// v4 multiLocalizedUnicodeType with one record per (language+country, text).
std::vector<uint8_t> MlucTag(const std::vector<std::pair<std::string, std::u16string>>& records)
{
    std::vector<uint8_t> tag;
    PutSignature(tag, "mluc");
    PutU32(tag, 0);
    PutU32(tag, static_cast<uint32_t>(records.size()));
    PutU32(tag, 12);
    size_t offset = 16 + records.size() * 12;
    for (const auto& [locale, text] : records)
    {
        PutSignature(tag, locale.c_str());
        PutU32(tag, static_cast<uint32_t>(text.size() * 2));
        PutU32(tag, static_cast<uint32_t>(offset));
        offset += text.size() * 2;
    }
    for (const auto& record : records)
    {
        for (char16_t ch : record.second)
        {
            tag.push_back(static_cast<uint8_t>(ch >> 8));
            tag.push_back(static_cast<uint8_t>(ch));
        }
    }
    return tag;
}

// A JPEG carrying `chunks` as APP2 ICC_PROFILE segments: (number, count, data).
std::vector<uint8_t> JpegWithIccChunks(
    const std::vector<std::tuple<uint8_t, uint8_t, std::vector<uint8_t>>>& chunks)
{
    std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00};
    for (const auto& [number, count, data] : chunks)
    {
        std::vector<uint8_t> payload = {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0,
                                        number, count};
        payload.insert(payload.end(), data.begin(), data.end());
        const size_t length = payload.size() + 2;
        jpeg.push_back(0xFF);
        jpeg.push_back(0xE2);
        jpeg.push_back(static_cast<uint8_t>(length >> 8));
        jpeg.push_back(static_cast<uint8_t>(length));
        jpeg.insert(jpeg.end(), payload.begin(), payload.end());
    }
    jpeg.insert(jpeg.end(), {0xFF, 0xDA, 0x00, 0x02});
    return jpeg;
}

// sRGB and Display P3 colorants as ICC profiles store them (D50-adapted).
constexpr double kSrgbRed[3] = {0.43607, 0.22249, 0.01392};
constexpr double kSrgbGreen[3] = {0.38515, 0.71687, 0.09708};
constexpr double kSrgbBlue[3] = {0.14307, 0.06061, 0.71410};
constexpr double kP3Red[3] = {0.51512, 0.24120, -0.00105};
constexpr double kP3Green[3] = {0.29198, 0.69225, 0.04189};
constexpr double kP3Blue[3] = {0.15710, 0.06657, 0.78407};

void RequireMatrix(const std::array<float, 9>& actual, const std::array<double, 9>& expected)
{
    for (size_t i = 0; i < 9; ++i)
    {
        INFO("element " << i);
        REQUIRE_THAT(actual[i], WithinAbs(expected[i], 2e-3));
    }
}

}  // namespace

TEST_CASE("An sRGB profile maps to the identity", "[icc]")
{
    const auto matrix = SrgbCurveProfileToSrgbMatrix(MakeProfile(kSrgbRed, kSrgbGreen, kSrgbBlue));
    REQUIRE(matrix.has_value());
    RequireMatrix(*matrix, {1, 0, 0, 0, 1, 0, 0, 0, 1});
}

TEST_CASE("Display P3 maps to the known P3-to-sRGB matrix", "[icc]")
{
    const auto matrix = SrgbCurveProfileToSrgbMatrix(MakeProfile(kP3Red, kP3Green, kP3Blue));
    REQUIRE(matrix.has_value());
    RequireMatrix(*matrix, {1.2249, -0.2247, 0.0,     //
                            -0.0420, 1.0419, 0.0,     //
                            -0.0197, -0.0786, 1.0979});
    // Rows sum to 1: white stays white.
    for (size_t row = 0; row < 3; ++row)
    {
        REQUIRE_THAT((*matrix)[row * 3] + (*matrix)[row * 3 + 1] + (*matrix)[row * 3 + 2],
                     WithinAbs(1.0, 2e-3));
    }
}

TEST_CASE("Profiles with other tone curves are not converted", "[icc]")
{
    const std::vector<double> gamma22 = {2.2};
    REQUIRE_FALSE(
        SrgbCurveProfileToSrgbMatrix(MakeProfile(kP3Red, kP3Green, kP3Blue, gamma22)).has_value());
}

TEST_CASE("Malformed profiles are rejected", "[icc]")
{
    REQUIRE_FALSE(SrgbCurveProfileToSrgbMatrix({}).has_value());

    auto profile = MakeProfile(kP3Red, kP3Green, kP3Blue);
    std::memcpy(profile.data() + 16, "CMYK", 4);
    REQUIRE_FALSE(SrgbCurveProfileToSrgbMatrix(profile).has_value());

    // Truncated inside the tag data.
    profile = MakeProfile(kP3Red, kP3Green, kP3Blue);
    profile.resize(profile.size() - 10);
    REQUIRE_FALSE(SrgbCurveProfileToSrgbMatrix(profile).has_value());
}

TEST_CASE("RGB profiles are recognized by their header", "[icc]")
{
    auto profile = MakeProfile(kP3Red, kP3Green, kP3Blue);
    REQUIRE(IsRgbProfile(profile));

    std::memcpy(profile.data() + 16, "CMYK", 4);
    REQUIRE_FALSE(IsRgbProfile(profile));

    profile = MakeProfile(kP3Red, kP3Green, kP3Blue);
    std::memcpy(profile.data() + 36, "xxxx", 4);
    REQUIRE_FALSE(IsRgbProfile(profile));
    REQUIRE_FALSE(IsRgbProfile({}));
}

TEST_CASE("Only sRGB in all but name skips conversion", "[icc]")
{
    REQUIRE(IsSrgbEquivalent(MakeProfile(kSrgbRed, kSrgbGreen, kSrgbBlue)));
    REQUIRE_FALSE(IsSrgbEquivalent(MakeProfile(kP3Red, kP3Green, kP3Blue)));
    // sRGB primaries on a plain 2.2 gamma still need the tone curve fixed.
    REQUIRE_FALSE(IsSrgbEquivalent(MakeProfile(kSrgbRed, kSrgbGreen, kSrgbBlue, {2.2})));
}

TEST_CASE("Descriptions come from v2 and v4 tags", "[icc]")
{
    REQUIRE(IccDescription(MakeProfile(kP3Red, kP3Green, kP3Blue, kSrgbCurve,
                                       {{"desc", DescTag("Display P3")}}))
            == L"Display P3");
    // English is preferred over the first record.
    REQUIRE(IccDescription(MakeProfile(kP3Red, kP3Green, kP3Blue, kSrgbCurve,
                                       {{"desc", MlucTag({{"jaJP", u"ディスプレイ"},
                                                          {"enUS", u"Display P3"}})}}))
            == L"Display P3");
    REQUIRE(IccDescription(MakeProfile(kP3Red, kP3Green, kP3Blue, kSrgbCurve,
                                       {{"desc", MlucTag({{"deDE", u"Anzeige"}})}}))
            == L"Anzeige");
    REQUIRE(IccDescription(MakeProfile(kP3Red, kP3Green, kP3Blue)).empty());
}

TEST_CASE("A JPEG's ICC profile is reassembled from its chunks", "[icc]")
{
    const std::vector<uint8_t> first = {1, 2, 3};
    const std::vector<uint8_t> second = {4, 5};

    REQUIRE(ReadJpegIccProfile(JpegWithIccChunks({{1, 1, first}})) == first);
    // Chunk numbers, not file order, decide the sequence.
    REQUIRE(ReadJpegIccProfile(JpegWithIccChunks({{2, 2, second}, {1, 2, first}}))
            == std::vector<uint8_t>{1, 2, 3, 4, 5});

    SECTION("incomplete or inconsistent chunks give nothing")
    {
        REQUIRE(ReadJpegIccProfile(JpegWithIccChunks({{1, 2, first}})).empty());
        REQUIRE(ReadJpegIccProfile(JpegWithIccChunks({{1, 2, first}, {2, 3, second}})).empty());
        REQUIRE(ReadJpegIccProfile(JpegWithIccChunks({{1, 1, first}, {1, 1, second}})).empty());
        REQUIRE(ReadJpegIccProfile(JpegWithIccChunks({{0, 1, first}})).empty());
    }
    SECTION("no profile")
    {
        REQUIRE(ReadJpegIccProfile(JpegWithIccChunks({})).empty());
        REQUIRE(ReadJpegIccProfile({}).empty());
    }
}

TEST_CASE("A sampled sRGB curve counts as sRGB", "[icc]")
{
    // Windows' own sRGB profile is v2 with a 1024-entry table.
    REQUIRE(IsSrgbEquivalent(
        MakeProfileWithTrc(kSrgbRed, kSrgbGreen, kSrgbBlue, CurvTag(1024, SrgbToLinear))));
    REQUIRE(IsSrgbEquivalent(
        MakeProfileWithTrc(kSrgbRed, kSrgbGreen, kSrgbBlue, CurvTag(4096, SrgbToLinear))));
    // A sampled 2.2 gamma is close, but not the same curve.
    REQUIRE_FALSE(IsSrgbEquivalent(MakeProfileWithTrc(
        kSrgbRed, kSrgbGreen, kSrgbBlue, CurvTag(1024, [](double v) { return std::pow(v, 2.2); }))));
    // A single entry is a plain gamma, and an empty table is the identity.
    std::vector<uint8_t> gamma = {'c', 'u', 'r', 'v', 0, 0, 0, 0, 0, 0, 0, 1, 0x02, 0x33};
    REQUIRE_FALSE(IsSrgbEquivalent(MakeProfileWithTrc(kSrgbRed, kSrgbGreen, kSrgbBlue, gamma)));
    std::vector<uint8_t> identity = {'c', 'u', 'r', 'v', 0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE_FALSE(IsSrgbEquivalent(MakeProfileWithTrc(kSrgbRed, kSrgbGreen, kSrgbBlue, identity)));
}
