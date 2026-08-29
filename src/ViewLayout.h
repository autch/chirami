#pragma once

#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

enum class ZoomMode
{
    Fit,         // shrink to fit the window, never upscale (scrollbars off)
    ActualSize,  // dot-by-dot
    Custom       // user-driven zoom factor
};

// Where and how large the image appears in the client area. panX/panY are
// the input pan clamped to [0, maxPan]; dest is derived from those values.
struct ViewLayout
{
    float scale = 0.0f;
    float displayWidth = 0.0f;   // image size * scale, whole pixels
    float displayHeight = 0.0f;
    float destX = 0.0f;          // top-left of the image in client coords
    float destY = 0.0f;
    float maxPanX = 0.0f;        // 0 when the image fits on that axis
    float maxPanY = 0.0f;
    float panX = 0.0f;
    float panY = 0.0f;

    D2D1_POINT_2F ClientToImage(float clientX, float clientY) const
    {
        return {(clientX - destX) / scale, (clientY - destY) / scale};
    }
};

// Scale for ActualSize (1) and Custom (`zoomScale`). Fit depends on the
// viewport and is computed inside ComputeViewLayout.
inline float FixedZoomScale(ZoomMode mode, float zoomScale)
{
    return mode == ZoomMode::ActualSize ? 1.0f : zoomScale;
}

// Whole pixels, so layout, scrollbar visibility, and window auto-fit agree;
// raw float products carry rounding noise (e.g. 640.00001) that would
// spuriously overflow an exactly-fitting client area.
inline float RoundedDisplayLength(uint32_t imagePixels, float scale)
{
    return std::round(static_cast<float>(imagePixels) * scale);
}

inline ViewLayout ComputeViewLayout(uint32_t imageWidth, uint32_t imageHeight,
                                    float clientWidth, float clientHeight, ZoomMode zoomMode,
                                    float zoomScale, float panX, float panY)
{
    ViewLayout layout;
    if (imageWidth == 0 || imageHeight == 0)
    {
        return layout;
    }

    const float imageW = static_cast<float>(imageWidth);
    const float imageH = static_cast<float>(imageHeight);

    switch (zoomMode)
    {
    case ZoomMode::Fit:
        // Shrink to fit while keeping the aspect ratio, never upscale.
        layout.scale = std::min({1.0f, clientWidth / imageW, clientHeight / imageH});
        break;
    case ZoomMode::ActualSize:
        layout.scale = 1.0f;
        break;
    case ZoomMode::Custom:
        layout.scale = zoomScale;
        break;
    }

    layout.displayWidth = RoundedDisplayLength(imageWidth, layout.scale);
    layout.displayHeight = RoundedDisplayLength(imageHeight, layout.scale);
    layout.maxPanX = std::max(0.0f, layout.displayWidth - clientWidth);
    layout.maxPanY = std::max(0.0f, layout.displayHeight - clientHeight);
    layout.panX = std::clamp(panX, 0.0f, layout.maxPanX);
    layout.panY = std::clamp(panY, 0.0f, layout.maxPanY);

    // Integer pixel offsets keep 100% display exactly dot-by-dot.
    layout.destX = layout.maxPanX > 0.0f
                       ? -std::round(layout.panX)
                       : std::round((clientWidth - layout.displayWidth) / 2.0f);
    layout.destY = layout.maxPanY > 0.0f
                       ? -std::round(layout.panY)
                       : std::round((clientHeight - layout.displayHeight) / 2.0f);
    return layout;
}
