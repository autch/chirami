#include "ImageTransform.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace
{

// Rotation and horizontal flip move whole pixels; doing it on a fixed-size
// integer keeps the loops simple and fast for both 4-byte (BGRA8) and
// 8-byte (RGBA16F) pixels.
template <typename Pixel>
void Rotate90Pixels(const PixelBuffer& source, PixelBuffer& result, bool clockwise)
{
    for (uint32_t y = 0; y < result.height; ++y)
    {
        auto* out = reinterpret_cast<Pixel*>(result.pixels.data() + size_t{y} * result.stride);
        if (clockwise)
        {
            const uint32_t sourceX = y;
            for (uint32_t x = 0; x < result.width; ++x)
            {
                const auto* row = reinterpret_cast<const Pixel*>(
                    source.pixels.data() + size_t{source.height - 1 - x} * source.stride);
                out[x] = row[sourceX];
            }
        }
        else
        {
            const uint32_t sourceX = source.width - 1 - y;
            for (uint32_t x = 0; x < result.width; ++x)
            {
                const auto* row = reinterpret_cast<const Pixel*>(source.pixels.data()
                                                                 + size_t{x} * source.stride);
                out[x] = row[sourceX];
            }
        }
    }
}

template <typename Pixel>
void FlipHorizontalPixels(PixelBuffer& image)
{
    for (uint32_t y = 0; y < image.height; ++y)
    {
        auto* row = reinterpret_cast<Pixel*>(image.pixels.data() + size_t{y} * image.stride);
        std::reverse(row, row + image.width);
    }
}

PixelBuffer Rotate90(const PixelBuffer& source, bool clockwise)
{
    PixelBuffer result;
    result.width = source.height;
    result.height = source.width;
    result.format = source.format;
    result.stride = result.width * result.BytesPerPixel();
    result.pixels.resize(source.pixels.size());

    if (source.format == PixelBuffer::Format::Rgba16F)
    {
        Rotate90Pixels<uint64_t>(source, result, clockwise);
    }
    else
    {
        Rotate90Pixels<uint32_t>(source, result, clockwise);
    }
    return result;
}

void FlipHorizontal(PixelBuffer& image)
{
    if (image.format == PixelBuffer::Format::Rgba16F)
    {
        FlipHorizontalPixels<uint64_t>(image);
    }
    else
    {
        FlipHorizontalPixels<uint32_t>(image);
    }
}

void FlipVertical(PixelBuffer& image)
{
    std::vector<uint8_t> scratch(image.stride);
    for (uint32_t y = 0; y < image.height / 2; ++y)
    {
        uint8_t* top = image.pixels.data() + size_t{y} * image.stride;
        uint8_t* bottom = image.pixels.data() + size_t{image.height - 1 - y} * image.stride;
        std::memcpy(scratch.data(), top, image.stride);
        std::memcpy(top, bottom, image.stride);
        std::memcpy(bottom, scratch.data(), image.stride);
    }
}

PixelBuffer Crop(const PixelBuffer& source, uint32_t x, uint32_t y, uint32_t width,
                 uint32_t height)
{
    PixelBuffer result;
    result.width = width;
    result.height = height;
    result.format = source.format;
    result.stride = width * result.BytesPerPixel();
    result.pixels.resize(size_t{result.stride} * height);

    const uint32_t bytesPerPixel = source.BytesPerPixel();
    for (uint32_t row = 0; row < height; ++row)
    {
        std::memcpy(result.pixels.data() + size_t{row} * result.stride,
                    source.pixels.data() + size_t{y + row} * source.stride
                        + size_t{x} * bytesPerPixel,
                    result.stride);
    }
    return result;
}

HRESULT Resize(IWICImagingFactory* factory, const PixelBuffer& source, uint32_t width,
               uint32_t height, PixelBuffer& out) noexcept
try
{
    const WICPixelFormatGUID wicFormat = source.WicPixelFormat();

    wil::com_ptr<IWICBitmap> bitmap;
    RETURN_IF_FAILED(factory->CreateBitmapFromMemory(
        source.width, source.height, wicFormat, source.stride,
        static_cast<UINT>(source.pixels.size()),
        const_cast<BYTE*>(source.pixels.data()), bitmap.put()));

    wil::com_ptr<IWICBitmapScaler> scaler;
    RETURN_IF_FAILED(factory->CreateBitmapScaler(scaler.put()));
    RETURN_IF_FAILED(
        scaler->Initialize(bitmap.get(), width, height, WICBitmapInterpolationModeFant));

    out.width = width;
    out.height = height;
    out.format = source.format;
    out.stride = width * out.BytesPerPixel();
    out.pixels.resize(size_t{out.stride} * height);
    RETURN_IF_FAILED(scaler->CopyPixels(nullptr, out.stride,
                                        static_cast<UINT>(out.pixels.size()),
                                        out.pixels.data()));
    return S_OK;
}
CATCH_RETURN()

// Maps a span of the base image onto the gain map, which covers the same
// area at its own (usually half) resolution. Rounds outward and keeps at
// least one pixel; the renderer stretches whatever remains over the whole
// base, so the sub-pixel slack is invisible.
void MapSpan(uint32_t start, uint32_t length, uint32_t baseSize, uint32_t mapSize,
             uint32_t& mapStart, uint32_t& mapLength)
{
    const uint64_t begin = uint64_t{start} * mapSize / baseSize;
    const uint64_t end = (uint64_t{start + length} * mapSize + baseSize - 1) / baseSize;
    mapStart = static_cast<uint32_t>(std::min<uint64_t>(begin, mapSize - 1));
    mapLength = static_cast<uint32_t>(
        std::clamp<uint64_t>(end, uint64_t{mapStart} + 1, mapSize) - mapStart);
}

// The gain map's size for a base resized from `baseSize` to `newBaseSize`.
uint32_t ScaledMapLength(uint32_t mapSize, uint32_t baseSize, uint32_t newBaseSize)
{
    const uint64_t scaled = (uint64_t{mapSize} * newBaseSize + baseSize / 2) / baseSize;
    return static_cast<uint32_t>(std::max<uint64_t>(scaled, 1));
}

// Copies everything but the pixels, so the result keeps its gain map
// headroom, color conversion and load-time orientation note.
LoadedImage WithPixels(const LoadedImage& source, PixelBuffer pixels)
{
    LoadedImage result;
    static_cast<PixelBuffer&>(result) = std::move(pixels);
    result.gainMap.headroom = source.gainMap.headroom;
    result.toSrgb = source.toSrgb;
    result.appliedOrientation = source.appliedOrientation;
    return result;
}

}  // namespace

// The gain map is transformed alongside the pixels so the highlights stay
// on the parts of the image they belong to. See DESIGN.md, "HDR gain map".

LoadedImage RotateImage90(const LoadedImage& source, bool clockwise)
{
    LoadedImage result = WithPixels(source, Rotate90(source, clockwise));
    if (source.gainMap)
    {
        result.gainMap.map = Rotate90(source.gainMap.map, clockwise);
    }
    return result;
}

void FlipImageHorizontal(LoadedImage& image)
{
    FlipHorizontal(image);
    if (image.gainMap)
    {
        FlipHorizontal(image.gainMap.map);
    }
}

void FlipImageVertical(LoadedImage& image)
{
    FlipVertical(image);
    if (image.gainMap)
    {
        FlipVertical(image.gainMap.map);
    }
}

LoadedImage CropImage(const LoadedImage& source, uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height)
{
    LoadedImage result = WithPixels(source, Crop(source, x, y, width, height));
    if (const PixelBuffer& map = source.gainMap.map; source.gainMap)
    {
        uint32_t mapX = 0, mapWidth = 0, mapY = 0, mapHeight = 0;
        MapSpan(x, width, source.width, map.width, mapX, mapWidth);
        MapSpan(y, height, source.height, map.height, mapY, mapHeight);
        result.gainMap.map = Crop(map, mapX, mapY, mapWidth, mapHeight);
    }
    return result;
}

// The gain map is left alone: it multiplies, and black stays black.
void FillRectBlack(LoadedImage& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    for (uint32_t row = 0; row < height; ++row)
    {
        if (image.format == PixelBuffer::Format::Rgba16F)
        {
            // Half floats: R=G=B=0.0, A=1.0 (0x3C00), little-endian words.
            auto* pixels = reinterpret_cast<uint64_t*>(image.pixels.data()
                                                       + size_t{y + row} * image.stride)
                           + x;
            std::fill_n(pixels, width, 0x3C00000000000000ull);
        }
        else
        {
            auto* pixels = reinterpret_cast<uint32_t*>(image.pixels.data()
                                                       + size_t{y + row} * image.stride)
                           + x;
            std::fill_n(pixels, width, 0xFF000000u);  // opaque black
        }
    }
}

HRESULT ResizeImage(IWICImagingFactory* factory, const LoadedImage& source, uint32_t width,
                    uint32_t height, LoadedImage& out) noexcept
{
    PixelBuffer pixels;
    RETURN_IF_FAILED(Resize(factory, source, width, height, pixels));
    LoadedImage result = WithPixels(source, std::move(pixels));
    if (const PixelBuffer& map = source.gainMap.map; source.gainMap)
    {
        RETURN_IF_FAILED(Resize(factory, map, ScaledMapLength(map.width, source.width, width),
                                ScaledMapLength(map.height, source.height, height),
                                result.gainMap.map));
    }
    out = std::move(result);
    return S_OK;
}
