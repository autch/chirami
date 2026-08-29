#include "PngText.h"
#include "MetadataText.h"
#include "StringUtil.h"
#include "resource.h"

#include <cstring>
#include <string_view>

namespace
{

uint32_t ReadBigEndian32(const uint8_t* bytes)
{
    return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8)
           | uint32_t{bytes[3]};
}

}  // namespace

void ParsePngTextChunks(const uint8_t* data, size_t size, std::vector<MetadataItem>& items)
{
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < sizeof(kSignature) + 12 || std::memcmp(data, kSignature, sizeof(kSignature)) != 0)
    {
        return;
    }

    const std::wstring compressedPlaceholder = LoadStringResource(IDS_META_COMPRESSED);
    size_t pos = sizeof(kSignature);
    while (pos + 12 <= size && items.size() < kMaxWicItems)
    {
        const uint32_t length = ReadBigEndian32(data + pos);
        const uint8_t* type = data + pos + 4;
        if (length > size - pos - 12)
        {
            break;  // truncated or corrupt; keep what was parsed so far
        }
        const char* payload = reinterpret_cast<const char*>(data + pos + 8);
        pos += 12ull + length;

        if (std::memcmp(type, "IEND", 4) == 0)
        {
            break;
        }
        const bool isText = std::memcmp(type, "tEXt", 4) == 0;
        const bool isIntl = std::memcmp(type, "iTXt", 4) == 0;
        const bool isCompressed = std::memcmp(type, "zTXt", 4) == 0;
        if (!isText && !isIntl && !isCompressed)
        {
            continue;
        }

        // All three start with keyword\0 (Latin-1, 1-79 chars).
        const std::string_view chunk(payload, length);
        const size_t keywordEnd = chunk.find('\0');
        if (keywordEnd == std::string_view::npos || keywordEnd == 0 || keywordEnd > 79)
        {
            continue;
        }
        std::wstring keyword = WidenBytes(chunk.data(), keywordEnd);

        std::wstring value;
        if (isText)
        {
            value = WidenBytes(chunk.data() + keywordEnd + 1, chunk.size() - keywordEnd - 1);
        }
        else if (isCompressed)
        {
            value = compressedPlaceholder;
        }
        else  // iTXt: keyword\0 compFlag compMethod langTag\0 translated\0 text
        {
            size_t cursor = keywordEnd + 1;
            if (cursor + 2 > chunk.size())
            {
                continue;
            }
            const bool payloadCompressed = chunk[cursor] != 0;
            cursor += 2;
            const size_t langEnd = chunk.find('\0', cursor);
            if (langEnd == std::string_view::npos)
            {
                continue;
            }
            const size_t translatedEnd = chunk.find('\0', langEnd + 1);
            if (translatedEnd == std::string_view::npos)
            {
                continue;
            }
            cursor = translatedEnd + 1;
            value = payloadCompressed ? compressedPlaceholder
                                      : WidenBytes(chunk.data() + cursor, chunk.size() - cursor);
        }
        items.push_back({MetadataGroup::PngText, std::move(keyword), ClampValue(std::move(value))});
    }
}
