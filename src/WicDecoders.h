#pragma once

#include "framework.h"

#include <string>
#include <unordered_set>

// Lowercase file extensions (".jpg", ...) claimed by all installed WIC
// decoders, so optional codecs (WebP, AVIF, HEIF, ...) are picked up
// automatically. Throws on enumeration failure.
std::unordered_set<std::wstring> QueryWicDecoderExtensions(IWICImagingFactory* factory);
