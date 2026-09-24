#pragma once

#include <cstdint>
#include <span>

// EXIF Orientation (tag 274) for JPEG, which neither libjpeg-turbo nor the
// WIC decoder applies. WTL-free for the tests. See DESIGN.md, "EXIF
// Orientation".

// The Orientation in a JPEG's APP1 Exif segment (IFD0), 1..8. Returns 1
// (normal) when there is none or the data is malformed.
uint16_t ReadJpegExifOrientation(std::span<const uint8_t> jpeg);

// What to do to the stored pixels to show them upright: the flips first,
// then the rotation. Values outside 1..8 come back as no-op steps.
struct OrientationSteps
{
    bool flipHorizontal = false;
    bool flipVertical = false;
    int quarterTurnsClockwise = 0;  // 0, 1 or 3

    bool IsIdentity() const
    {
        return !flipHorizontal && !flipVertical && quarterTurnsClockwise == 0;
    }
};

OrientationSteps StepsForOrientation(uint16_t orientation);
