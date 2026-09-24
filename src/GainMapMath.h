#pragma once

#include <span>

// Gain-map arithmetic shared by the renderer and the tests. WTL-free so it
// links into chirami_tests (see DESIGN.md, "Test policy").
//
// Follows Skia's handling of Apple's gain map (SkGainmapShader, the kApple
// branch): the stored 8-bit gain g in 0..1 becomes the linear boost
//     boost(g) = 1 + (headroom - 1) * g^kAppleGainGamma
// and the display takes it partially, in log space:
//     hdr = sdr_linear * boost(g)^weight
// See DESIGN.md, "HDR gain map".

inline constexpr float kAppleGainGamma = 1.961f;

// How much of the gain the display gets: 0 shows the SDR base untouched, 1
// the full HDR rendition. `displayHeadroom` is the display's peak over its
// SDR white (1 on SDR displays); `contentHeadroom` is the image's.
float GainMapWeight(float displayHeadroom, float contentHeadroom);

// Fills `table` with boost(g)^weight for g evenly spaced over 0..1, divided
// by the largest entry so every value lies in 0..1 (a Direct2D table
// transfer clamps to that range). Returns the divisor, which the caller
// multiplies back in afterwards. `table` needs at least two entries.
float BuildAppleGainTable(float contentHeadroom, float weight, std::span<float> table);
