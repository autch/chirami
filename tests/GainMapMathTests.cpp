#include "GainMapMath.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE("Gain weight is zero without display headroom", "[gainmap]")
{
    REQUIRE(GainMapWeight(1.0f, 4.0f) == 0.0f);
    REQUIRE(GainMapWeight(0.5f, 4.0f) == 0.0f);
}

TEST_CASE("Gain weight is zero for content without headroom", "[gainmap]")
{
    REQUIRE(GainMapWeight(4.0f, 1.0f) == 0.0f);
    REQUIRE(GainMapWeight(4.0f, 0.0f) == 0.0f);
}

TEST_CASE("Gain weight interpolates in log space and saturates", "[gainmap]")
{
    // Half the content's stops -> half the weight.
    REQUIRE_THAT(GainMapWeight(2.0f, 4.0f), WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(GainMapWeight(4.0f, 4.0f), WithinAbs(1.0, 1e-6));
    REQUIRE(GainMapWeight(16.0f, 4.0f) == 1.0f);
}

TEST_CASE("Gain table spans 1 to the headroom at full weight", "[gainmap]")
{
    std::vector<float> table(256);
    const float divisor = BuildAppleGainTable(4.0f, 1.0f, table);
    REQUIRE_THAT(divisor, WithinAbs(4.0, 1e-5));
    // g = 0 leaves the pixel alone, g = 1 reaches the headroom.
    REQUIRE_THAT(table.front() * divisor, WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(table.back() * divisor, WithinAbs(4.0, 1e-5));
    for (size_t i = 1; i < table.size(); ++i)
    {
        REQUIRE(table[i] >= table[i - 1]);
        REQUIRE(table[i] <= 1.0f);
    }
}

TEST_CASE("Gain table follows Apple's curve", "[gainmap]")
{
    std::vector<float> table(3);  // g = 0, 0.5, 1
    const float divisor = BuildAppleGainTable(5.0f, 1.0f, table);
    const double expected = 1.0 + 4.0 * std::pow(0.5, kAppleGainGamma);
    REQUIRE_THAT(table[1] * divisor, WithinAbs(expected, 1e-5));
}

TEST_CASE("Partial weight takes the boost to that power", "[gainmap]")
{
    std::vector<float> table(2);
    const float divisor = BuildAppleGainTable(4.0f, 0.5f, table);
    REQUIRE_THAT(table.back() * divisor, WithinAbs(2.0, 1e-5));  // 4^0.5
}

TEST_CASE("Zero weight is the identity", "[gainmap]")
{
    std::vector<float> table(16);
    const float divisor = BuildAppleGainTable(4.0f, 0.0f, table);
    REQUIRE(divisor == 1.0f);
    for (float value : table)
    {
        REQUIRE_THAT(value, WithinAbs(1.0, 1e-6));
    }
}
