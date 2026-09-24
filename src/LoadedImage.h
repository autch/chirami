#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <wincodec.h>

// The decoded image exceeds the pixel budget or the render target's
// maximum bitmap size. Tiling raises the display limit; this remains the
// decode-side cap.
inline constexpr long kHrImageTooLarge = static_cast<long>(0x80040200);  // FACILITY_ITF

// Generous sanity cap on decoded pixel data, checked before allocation so a
// hostile header cannot request an absurd buffer.
inline constexpr uint64_t kMaxPixelBytes = 2ull << 30;  // 2 GiB (~23K x 23K x 4)

// A block of decoded pixels, premultiplied alpha.
// - Bgra8: 32bpp sRGB (GUID_WICPixelFormat32bppPBGRA), the common case
// - Rgba16F: 64bpp scRGB half floats (GUID_WICPixelFormat64bppPRGBAHalf)
//   for high-precision/HDR sources
struct PixelBuffer
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

    // Pixel format used when wrapping these pixels as a WIC bitmap.
    WICPixelFormatGUID WicPixelFormat() const
    {
        return format == Format::Rgba16F ? GUID_WICPixelFormat64bppPRGBAHalf
                                         : GUID_WICPixelFormat32bppPBGRA;
    }
};

// An HDR gain map riding along with an SDR base image (iPhone HEIC). The
// map is kept as Bgra8 with the gain in R=G=B, so every transform and the
// GPU upload treat it like any other image. It is usually smaller than the
// base (about half size) and is stretched over the whole base when drawn.
// See DESIGN.md, "HDR gain map".
struct HdrGainMap
{
    PixelBuffer map;
    float headroom = 1.0f;  // linear ratio of the HDR rendition's peak to SDR white

    explicit operator bool() const { return static_cast<bool>(map); }
};

// Row-major 3x3 matrix taking linear base-image RGB to linear sRGB (scRGB)
// primaries.
using ColorMatrix3 = std::array<float, 9>;

// What the loader hands to the UI: the pixels, plus an optional gain map.
// Only the pixels are ever saved; the gain map is a display-time effect.
struct LoadedImage : PixelBuffer
{
    HdrGainMap gainMap;
    // Set only for gain-map images, whose base is Display P3 rather than
    // sRGB. Other images are drawn without gamut conversion as before.
    std::optional<ColorMatrix3> toSrgb;

    // Bytes held, for cache accounting.
    size_t TotalBytes() const { return pixels.size() + gainMap.map.pixels.size(); }
};
