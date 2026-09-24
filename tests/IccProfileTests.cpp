#include "IccProfile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
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

// A matrix/TRC display profile with the given D50-adapted colorants.
std::vector<uint8_t> MakeProfile(const double (&red)[3], const double (&green)[3],
                                 const double (&blue)[3],
                                 const std::vector<double>& curve = kSrgbCurve)
{
    const std::vector<std::pair<std::string, std::vector<uint8_t>>> tags = {
        {"rXYZ", XyzTag(red[0], red[1], red[2])},
        {"gXYZ", XyzTag(green[0], green[1], green[2])},
        {"bXYZ", XyzTag(blue[0], blue[1], blue[2])},
        {"rTRC", ParaTag(curve)},
        {"gTRC", ParaTag(curve)},
        {"bTRC", ParaTag(curve)},
    };

    std::vector<uint8_t> profile(128, 0);
    std::memcpy(profile.data() + 12, "mntr", 4);
    std::memcpy(profile.data() + 16, "RGB ", 4);
    std::memcpy(profile.data() + 20, "XYZ ", 4);
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
