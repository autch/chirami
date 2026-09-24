#include "ExifOrientation.h"

#include <cstring>

namespace
{

constexpr uint16_t kNormal = 1;
constexpr uint16_t kOrientationTag = 274;
constexpr uint16_t kTypeShort = 3;

// A bounds-checked reader over the TIFF structure inside the Exif segment.
class TiffReader
{
public:
    TiffReader(std::span<const uint8_t> data, bool bigEndian) : m_data(data), m_bigEndian(bigEndian)
    {
    }

    bool U16(size_t offset, uint16_t& value) const
    {
        if (offset > m_data.size() || m_data.size() - offset < 2)
        {
            return false;
        }
        const uint8_t* p = m_data.data() + offset;
        value = m_bigEndian ? static_cast<uint16_t>((p[0] << 8) | p[1])
                            : static_cast<uint16_t>(p[0] | (p[1] << 8));
        return true;
    }

    bool U32(size_t offset, uint32_t& value) const
    {
        uint16_t first = 0;
        uint16_t second = 0;
        if (!U16(offset, first) || !U16(offset + 2, second))
        {
            return false;
        }
        value = m_bigEndian ? (uint32_t{first} << 16) | second : (uint32_t{second} << 16) | first;
        return true;
    }

private:
    std::span<const uint8_t> m_data;
    bool m_bigEndian;
};

uint16_t OrientationFromTiff(std::span<const uint8_t> tiff)
{
    if (tiff.size() < 8)
    {
        return kNormal;
    }
    bool bigEndian = false;
    if (tiff[0] == 'M' && tiff[1] == 'M')
    {
        bigEndian = true;
    }
    else if (!(tiff[0] == 'I' && tiff[1] == 'I'))
    {
        return kNormal;
    }
    const TiffReader reader(tiff, bigEndian);
    uint16_t magic = 0;
    uint32_t ifdOffset = 0;
    uint16_t count = 0;
    if (!reader.U16(2, magic) || magic != 42 || !reader.U32(4, ifdOffset)
        || !reader.U16(ifdOffset, count))
    {
        return kNormal;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t entry = size_t{ifdOffset} + 2 + size_t{i} * 12;
        uint16_t tag = 0;
        uint16_t type = 0;
        uint32_t valueCount = 0;
        uint16_t value = 0;
        if (!reader.U16(entry, tag) || !reader.U16(entry + 2, type)
            || !reader.U32(entry + 4, valueCount) || !reader.U16(entry + 8, value))
        {
            return kNormal;
        }
        if (tag == kOrientationTag)
        {
            // A single SHORT sits left-justified in the value field.
            return (type == kTypeShort && valueCount == 1 && value >= 1 && value <= 8) ? value
                                                                                      : kNormal;
        }
    }
    return kNormal;
}

}  // namespace

uint16_t ReadJpegExifOrientation(std::span<const uint8_t> jpeg)
{
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
    {
        return kNormal;
    }
    size_t pos = 2;
    while (pos + 4 <= jpeg.size())
    {
        if (jpeg[pos] != 0xFF)
        {
            return kNormal;  // lost sync; not worth guessing
        }
        const uint8_t marker = jpeg[pos + 1];
        if (marker == 0xFF)
        {
            ++pos;  // fill byte
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
        {
            pos += 2;  // standalone markers carry no length
            continue;
        }
        if (marker == 0xDA || marker == 0xD9)
        {
            return kNormal;  // image data or end: metadata comes before these
        }
        const size_t length = (size_t{jpeg[pos + 2]} << 8) | jpeg[pos + 3];
        if (length < 2 || length > jpeg.size() - pos - 2)
        {
            return kNormal;
        }
        const auto payload = jpeg.subspan(pos + 4, length - 2);
        constexpr uint8_t kExifHeader[] = {'E', 'x', 'i', 'f', 0, 0};
        if (marker == 0xE1 && payload.size() >= sizeof(kExifHeader)
            && std::memcmp(payload.data(), kExifHeader, sizeof(kExifHeader)) == 0)
        {
            return OrientationFromTiff(payload.subspan(sizeof(kExifHeader)));
        }
        pos += 2 + length;
    }
    return kNormal;
}

OrientationSteps StepsForOrientation(uint16_t orientation)
{
    // EXIF names where the stored 0th row and 0th column belong on screen.
    switch (orientation)
    {
    case 2:  // row 0 top, column 0 right
        return {true, false, 0};
    case 3:  // row 0 bottom, column 0 right: 180 degrees as two flips
        return {true, true, 0};
    case 4:  // row 0 bottom, column 0 left
        return {false, true, 0};
    case 5:  // row 0 left, column 0 top: transpose
        return {true, false, 3};
    case 6:  // row 0 right, column 0 top
        return {false, false, 1};
    case 7:  // row 0 right, column 0 bottom: transverse
        return {true, false, 1};
    case 8:  // row 0 left, column 0 bottom
        return {false, false, 3};
    default:
        return {};
    }
}
