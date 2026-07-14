#include "ImageTransform.h"

#include <algorithm>
#include <cstring>

namespace
{

// Pixels are 4-byte BGRA words; stride == width * 4 by construction.
const uint32_t* PixelRow(const LoadedImage& image, uint32_t y)
{
    return reinterpret_cast<const uint32_t*>(image.pixels.data() + size_t{y} * image.stride);
}

uint32_t* PixelRow(LoadedImage& image, uint32_t y)
{
    return reinterpret_cast<uint32_t*>(image.pixels.data() + size_t{y} * image.stride);
}

}  // namespace

LoadedImage RotateImage90(const LoadedImage& source, bool clockwise)
{
    LoadedImage result;
    result.width = source.height;
    result.height = source.width;
    result.stride = result.width * 4;
    result.pixels.resize(source.pixels.size());

    for (uint32_t y = 0; y < result.height; ++y)
    {
        uint32_t* out = PixelRow(result, y);
        if (clockwise)
        {
            // dst(x, y) = src(y', x') with the source column read bottom-up
            const uint32_t sourceX = y;
            for (uint32_t x = 0; x < result.width; ++x)
            {
                out[x] = PixelRow(source, source.height - 1 - x)[sourceX];
            }
        }
        else
        {
            const uint32_t sourceX = source.width - 1 - y;
            for (uint32_t x = 0; x < result.width; ++x)
            {
                out[x] = PixelRow(source, x)[sourceX];
            }
        }
    }
    return result;
}

void FlipImageHorizontal(LoadedImage& image)
{
    for (uint32_t y = 0; y < image.height; ++y)
    {
        uint32_t* row = PixelRow(image, y);
        std::reverse(row, row + image.width);
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
    result.stride = width * 4;
    result.pixels.resize(size_t{result.stride} * height);

    for (uint32_t row = 0; row < height; ++row)
    {
        std::memcpy(result.pixels.data() + size_t{row} * result.stride,
                    source.pixels.data() + size_t{y + row} * source.stride + size_t{x} * 4,
                    result.stride);
    }
    return result;
}

void FillRectBlack(LoadedImage& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    for (uint32_t row = 0; row < height; ++row)
    {
        auto* pixels = reinterpret_cast<uint32_t*>(image.pixels.data()
                                                   + size_t{y + row} * image.stride)
                       + x;
        std::fill_n(pixels, width, 0xFF000000u);  // opaque black
    }
}

HRESULT ResizeImage(IWICImagingFactory* factory, const LoadedImage& source, uint32_t width,
                    uint32_t height, LoadedImage& out) noexcept
try
{
    wil::com_ptr<IWICBitmap> bitmap;
    RETURN_IF_FAILED(factory->CreateBitmapFromMemory(
        source.width, source.height, GUID_WICPixelFormat32bppPBGRA, source.stride,
        static_cast<UINT>(source.pixels.size()),
        const_cast<BYTE*>(source.pixels.data()), bitmap.put()));

    wil::com_ptr<IWICBitmapScaler> scaler;
    RETURN_IF_FAILED(factory->CreateBitmapScaler(scaler.put()));
    RETURN_IF_FAILED(
        scaler->Initialize(bitmap.get(), width, height, WICBitmapInterpolationModeFant));

    out.width = width;
    out.height = height;
    out.stride = width * 4;
    out.pixels.resize(size_t{out.stride} * height);
    RETURN_IF_FAILED(scaler->CopyPixels(nullptr, out.stride,
                                        static_cast<UINT>(out.pixels.size()),
                                        out.pixels.data()));
    return S_OK;
}
CATCH_RETURN()
