#pragma once

#include "framework.h"
#include "LoadedImage.h"

#include <cstddef>
#include <cstdint>

// Optional libjpeg-turbo decoder using the TurboJPEG 3 API. turbojpeg.dll is
// loaded at runtime from the executable's own directory only (full path, no
// search-path lookup); when it is absent or fails, callers fall back to WIC.
namespace TurboJpeg
{

// True when turbojpeg.dll was found next to the exe and its entry points
// resolved. Loading happens once on first call; thread-safe.
bool IsAvailable();

bool LooksLikeJpeg(const uint8_t* data, size_t size);

// Decodes a whole JPEG stream into 32bpp PBGRA (JPEG is opaque, so straight
// BGRA from TurboJPEG is already premultiplied).
HRESULT Decode(const uint8_t* data, size_t size, LoadedImage& out) noexcept;

}  // namespace TurboJpeg
