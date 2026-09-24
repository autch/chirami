#include "GainMapMath.h"

#include <algorithm>
#include <cmath>

float GainMapWeight(float displayHeadroom, float contentHeadroom)
{
    if (!(displayHeadroom > 1.0f) || !(contentHeadroom > 1.0f))
    {
        return 0.0f;
    }
    const float weight = std::log(displayHeadroom) / std::log(contentHeadroom);
    return std::clamp(weight, 0.0f, 1.0f);
}

float BuildAppleGainTable(float contentHeadroom, float weight, std::span<float> table)
{
    const float headroom = std::max(contentHeadroom, 1.0f);
    const size_t last = table.size() - 1;
    for (size_t i = 0; i < table.size(); ++i)
    {
        const float g = static_cast<float>(i) / static_cast<float>(last);
        const float boost = 1.0f + (headroom - 1.0f) * std::pow(g, kAppleGainGamma);
        table[i] = std::pow(boost, weight);
    }
    // boost is monotonic in g, so the last entry is the largest.
    const float divisor = std::max(table[last], 1.0f);
    for (float& value : table)
    {
        value /= divisor;
    }
    return divisor;
}
