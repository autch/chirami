#pragma once

#include "framework.h"
#include "LoadedImage.h"

#include <vector>

// One frame of an animated image, pre-composited to the full canvas.
struct AnimationFrame
{
    uint32_t delayMs = 100;
    LoadedImage image;
};

// Decodes every frame and composites it onto a persistent canvas, honoring
// the GIF frame rectangle and disposal method. Other containers (animated
// WebP via the OS codec) supply at least a frame delay; frames there are
// treated as full-canvas overlays, which matches how encoders commonly
// write them. Failure (odd metadata, over budget) is reported so the
// caller can fall back to a still first frame.
HRESULT DecodeAnimation(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, UINT frameCount,
                        std::vector<AnimationFrame>& outFrames);
