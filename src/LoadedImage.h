#pragma once

#include <cstdint>
#include <vector>

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
