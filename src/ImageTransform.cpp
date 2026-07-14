#include "ImageTransform.h"

#include <algorithm>
#include <cstring>

namespace
{

// Rotation and horizontal flip move whole pixels; doing it on a fixed-size
// integer keeps the loops simple and fast for both 4-byte (BGRA8) and
// 8-byte (RGBA16F) pixels.
template <typename Pixel>
void Rotate90Pixels(const LoadedImage& source, LoadedImage& result, bool clockwise)
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
void FlipHorizontalPixels(LoadedImage& image)
{
    for (uint32_t y = 0; y < image.height; ++y)
    {
        auto* row = reinterpret_cast<Pixel*>(image.pixels.data() + size_t{y} * image.stride);
        std::reverse(row, row + image.width);
    }
}

}  // namespace

LoadedImage RotateImage90(const LoadedImage& source, bool clockwise)
{
    LoadedImage result;
    result.width = source.height;
    result.height = source.width;
    result.format = source.format;
    result.stride = result.width * result.BytesPerPixel();
    result.pixels.resize(source.pixels.size());

    if (source.format == LoadedImage::Format::Rgba16F)
    {
        Rotate90Pixels<uint64_t>(source, result, clockwise);
    }
    else
    {
        Rotate90Pixels<uint32_t>(source, result, clockwise);
    }
    return result;
}

void FlipImageHorizontal(LoadedImage& image)
{
    if (image.format == LoadedImage::Format::Rgba16F)
    {
        FlipHorizontalPixels<uint64_t>(image);
    }
    else
    {
        FlipHorizontalPixels<uint32_t>(image);
    }
}

void FlipImageVertical(LoadedImage& image)
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

LoadedImage CropImage(const LoadedImage& source, uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height)
{
    LoadedImage result;
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

void FillRectBlack(LoadedImage& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    for (uint32_t row = 0; row < height; ++row)
    {
        if (image.format == LoadedImage::Format::Rgba16F)
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
try
{
    const bool half = source.format == LoadedImage::Format::Rgba16F;
    const WICPixelFormatGUID wicFormat =
        half ? GUID_WICPixelFormat64bppPRGBAHalf : GUID_WICPixelFormat32bppPBGRA;

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
