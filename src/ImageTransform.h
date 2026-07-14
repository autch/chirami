#pragma once

#include "LoadedImage.h"

// CPU-side pixel transforms. They bake the change into the pixels so the
// result is what gets saved, not just a display effect. Runs on the calling
// thread; even an 8K image transposes in well under a second.

LoadedImage RotateImage90(const LoadedImage& source, bool clockwise);
void FlipImageHorizontal(LoadedImage& image);
void FlipImageVertical(LoadedImage& image);

// Rectangle arguments are clamped to the image by the caller.
LoadedImage CropImage(const LoadedImage& source, uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height);
void FillRectBlack(LoadedImage& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
