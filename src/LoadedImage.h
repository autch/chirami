#pragma once

#include <cstdint>
#include <vector>

// The decoded image exceeds the pixel budget or the render target's
// maximum bitmap size. Tiling raises the display limit; this remains the
// decode-side cap.
inline constexpr long kHrImageTooLarge = static_cast<long>(0x80040200);  // FACILITY_ITF

// Generous sanity cap on decoded pixel data, checked before allocation so a
// hostile header cannot request an absurd buffer.
inline constexpr uint64_t kMaxPixelBytes = 2ull << 30;  // 2 GiB (~23K x 23K x 4)

// Decoded image pixels, premultiplied alpha.
// - Bgra8: 32bpp sRGB (GUID_WICPixelFormat32bppPBGRA), the common case
// - Rgba16F: 64bpp scRGB half floats (GUID_WICPixelFormat64bppPRGBAHalf)
//   for high-precision/HDR sources
struct LoadedImage
{
    enum class Format : uint8_t
    {
        Bgra8,
        Rgba16F,
    };

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;          // bytes per row (width * BytesPerPixel())
    Format format = Format::Bgra8;
    std::vector<uint8_t> pixels;  // stride * height bytes

    uint32_t BytesPerPixel() const { return format == Format::Rgba16F ? 8 : 4; }
    explicit operator bool() const { return !pixels.empty(); }
};
