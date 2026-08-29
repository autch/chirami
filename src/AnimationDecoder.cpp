#include "AnimationDecoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{

// Budget for all pre-composited animation frames together; an animation
// beyond it falls back to a static first frame.
constexpr uint64_t kMaxAnimationBytes = 1ull << 30;  // 1 GiB

// Clamp near-zero frame delays the way browsers do.
uint32_t NormalizeDelay(uint32_t delayMs)
{
    return delayMs < 20 ? 100 : delayMs;
}

bool TryReadUInt(IWICMetadataQueryReader* reader, PCWSTR name, uint32_t& value)
{
    if (reader == nullptr)
    {
        return false;
    }
    wil::unique_prop_variant var;
    if (FAILED(reader->GetMetadataByName(name, &var)))
    {
        return false;
    }
    switch (var.vt)
    {
    case VT_UI1:
        value = var.bVal;
        return true;
    case VT_UI2:
        value = var.uiVal;
        return true;
    case VT_UI4:
        value = var.ulVal;
        return true;
    default:
        return false;
    }
}

// Composites `source` (premultiplied BGRA, width x height) over `canvas` at
// (left, top). Alpha-over with premultiplied pixels is a simple add-blend.
void AlphaBlit(LoadedImage& canvas, const std::vector<uint8_t>& source, uint32_t sourceStride,
               uint32_t width, uint32_t height, uint32_t left, uint32_t top)
{
    const uint32_t right = std::min(canvas.width, left + width);
    const uint32_t bottom = std::min(canvas.height, top + height);
    for (uint32_t y = top; y < bottom; ++y)
    {
        const uint8_t* src = source.data() + size_t{y - top} * sourceStride;
        uint8_t* dst = canvas.pixels.data() + size_t{y} * canvas.stride + size_t{left} * 4;
        for (uint32_t x = left; x < right; ++x, src += 4, dst += 4)
        {
            const uint8_t alpha = src[3];
            if (alpha == 0xFF)
            {
                std::memcpy(dst, src, 4);
            }
            else if (alpha != 0)
            {
                const uint32_t inverse = 255u - alpha;
                dst[0] = static_cast<uint8_t>(src[0] + dst[0] * inverse / 255u);
                dst[1] = static_cast<uint8_t>(src[1] + dst[1] * inverse / 255u);
                dst[2] = static_cast<uint8_t>(src[2] + dst[2] * inverse / 255u);
                dst[3] = static_cast<uint8_t>(src[3] + dst[3] * inverse / 255u);
            }
        }
    }
}

void ClearRect(LoadedImage& canvas, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
    const uint32_t right = std::min(canvas.width, left + width);
    const uint32_t bottom = std::min(canvas.height, top + height);
    for (uint32_t y = top; y < bottom; ++y)
    {
        std::memset(canvas.pixels.data() + size_t{y} * canvas.stride + size_t{left} * 4, 0,
                    size_t{right - left} * 4);
    }
}

}  // namespace

HRESULT DecodeAnimation(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, UINT frameCount,
                        std::vector<AnimationFrame>& outFrames)
{
    // Canvas size: GIF logical screen if present, else the first frame.
    uint32_t canvasWidth = 0;
    uint32_t canvasHeight = 0;
    {
        wil::com_ptr<IWICMetadataQueryReader> containerReader;
        (void)decoder->GetMetadataQueryReader(containerReader.put());
        if (!TryReadUInt(containerReader.get(), L"/logscrdesc/Width", canvasWidth)
            || !TryReadUInt(containerReader.get(), L"/logscrdesc/Height", canvasHeight))
        {
            canvasWidth = 0;
            canvasHeight = 0;
        }
    }

    LoadedImage canvas;
    LoadedImage previousCanvas;  // for GIF disposal 3 (restore previous)
    std::vector<AnimationFrame> frames;

    for (UINT index = 0; index < frameCount; ++index)
    {
        wil::com_ptr<IWICBitmapFrameDecode> frame;
        RETURN_IF_FAILED(decoder->GetFrame(index, frame.put()));

        // Per-frame metadata; every read has a safe fallback.
        uint32_t left = 0;
        uint32_t top = 0;
        uint32_t delayMs = 100;
        uint32_t disposal = 0;
        {
            wil::com_ptr<IWICMetadataQueryReader> reader;
            (void)frame->GetMetadataQueryReader(reader.put());
            uint32_t value = 0;
            if (TryReadUInt(reader.get(), L"/grctlext/Delay", value))
            {
                delayMs = value * 10;  // GIF: 1/100s units
            }
            else if (TryReadUInt(reader.get(), L"/ANMF/FrameDuration", value))
            {
                delayMs = value;  // WebP: milliseconds
            }
            (void)TryReadUInt(reader.get(), L"/imgdesc/Left", left);
            (void)TryReadUInt(reader.get(), L"/imgdesc/Top", top);
            (void)TryReadUInt(reader.get(), L"/grctlext/Disposal", disposal);
        }

        wil::com_ptr<IWICFormatConverter> converter;
        RETURN_IF_FAILED(factory->CreateFormatConverter(converter.put()));
        RETURN_IF_FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom));
        UINT frameWidth = 0;
        UINT frameHeight = 0;
        RETURN_IF_FAILED(converter->GetSize(&frameWidth, &frameHeight));
        RETURN_HR_IF(WINCODEC_ERR_BADIMAGE, frameWidth == 0 || frameHeight == 0);

        if (canvas.pixels.empty())
        {
            if (canvasWidth == 0 || canvasHeight == 0)
            {
                canvasWidth = frameWidth;
                canvasHeight = frameHeight;
            }
            const uint64_t canvasBytes = uint64_t{canvasWidth} * canvasHeight * 4;
            RETURN_HR_IF(kHrImageTooLarge, canvasBytes > kMaxPixelBytes);
            RETURN_HR_IF(E_OUTOFMEMORY, canvasBytes * frameCount > kMaxAnimationBytes);
            canvas.width = canvasWidth;
            canvas.height = canvasHeight;
            canvas.stride = canvasWidth * 4;
            canvas.pixels.assign(size_t{canvas.stride} * canvasHeight, 0);
        }
        if (left >= canvas.width || top >= canvas.height)
        {
            left = 0;
            top = 0;
        }

        std::vector<uint8_t> pixels(size_t{frameWidth} * 4 * frameHeight);
        RETURN_IF_FAILED(converter->CopyPixels(nullptr, frameWidth * 4,
                                               static_cast<UINT>(pixels.size()), pixels.data()));

        if (disposal == 3)
        {
            previousCanvas = canvas;
        }
        AlphaBlit(canvas, pixels, frameWidth * 4, frameWidth, frameHeight, left, top);

        AnimationFrame composed;
        composed.delayMs = NormalizeDelay(delayMs);
        composed.image = canvas;  // snapshot
        frames.push_back(std::move(composed));

        switch (disposal)
        {
        case 2:  // restore background: clear the frame's area
            ClearRect(canvas, left, top, frameWidth, frameHeight);
            break;
        case 3:  // restore what was there before this frame
            if (!previousCanvas.pixels.empty())
            {
                canvas = previousCanvas;
            }
            break;
        default:
            break;  // 0/1: leave the frame in place
        }
    }

    outFrames = std::move(frames);
    return S_OK;
}
