#pragma once

#include "ImageMetadata.h"

#include <cstdint>
#include <vector>

// PNG tEXt / iTXt / zTXt. Parsed from the file bytes because WIC has no
// iTXt reader, and AI generators store non-ASCII prompts in iTXt.
// Compressed payloads (zTXt and compressed iTXt) get a placeholder so we
// do not take a zlib dependency. Appends to `items`; respects kMaxWicItems.
void ParsePngTextChunks(const uint8_t* data, size_t size, std::vector<MetadataItem>& items);
