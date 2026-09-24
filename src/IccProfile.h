#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

// Minimal ICC reading for matrix/TRC RGB profiles. WTL-free for the tests.

// Row-major 3x3 matrix from the profile's linear RGB to linear sRGB, for a
// profile whose tone curves are all the sRGB curve (as Display P3 is).
// Anything else - other curves, LUT-based profiles, malformed data - gives
// nullopt, and the caller draws the pixels unconverted as before.
std::optional<std::array<float, 9>> SrgbCurveProfileToSrgbMatrix(std::span<const uint8_t> icc);
