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

// Decoded image pixels in 32bpp premultiplied BGRA
// (GUID_WICPixelFormat32bppPBGRA), the format ID2D1HwndRenderTarget expects.
struct LoadedImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;          // bytes per row (width * 4)
    std::vector<uint8_t> pixels;  // stride * height bytes

    explicit operator bool() const { return !pixels.empty(); }
};
