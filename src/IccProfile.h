#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Minimal ICC reading: enough to decide whether an embedded profile needs
// converting, to name it, and to pull it out of a JPEG. The conversion
// itself is Direct2D's color management effect. WTL-free for the tests.
// See DESIGN.md, "Color profiles".

// True for a well-formed profile header ('acsp') whose data color space is
// RGB. Only these are applied to (and embedded with) the RGB pixels chirami
// holds; CMYK or gray profiles describe pixels WIC has already turned into
// RGB without them.
bool IsRgbProfile(std::span<const uint8_t> icc);

// Row-major 3x3 matrix from the profile's linear RGB to linear sRGB, for a
// matrix/TRC profile whose tone curves are all the sRGB curve, parametric
// (as Display P3 has) or sampled (as Windows' sRGB has). Anything else -
// other curves, LUT-based profiles, malformed data - gives nullopt.
std::optional<std::array<float, 9>> SrgbCurveProfileToSrgbMatrix(std::span<const uint8_t> icc);

// True when the profile is sRGB in all but name (sRGB curve and sRGB
// primaries), so drawing the pixels unconverted is already right.
bool IsSrgbEquivalent(std::span<const uint8_t> icc);

// The profile's description ('desc' tag, v2 text or v4 multi-localized,
// English preferred), or empty if there is none.
std::wstring IccDescription(std::span<const uint8_t> icc);

// The ICC profile embedded in a JPEG's APP2 "ICC_PROFILE" segments, which
// may be split over several markers. Empty if there is none or the chunks
// do not add up.
std::vector<uint8_t> ReadJpegIccProfile(std::span<const uint8_t> jpeg);
