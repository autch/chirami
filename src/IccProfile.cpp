#include "IccProfile.h"

#include <cmath>
#include <cstring>

namespace
{

using Matrix = std::array<double, 9>;

constexpr size_t kHeaderSize = 128;
constexpr size_t kTagEntrySize = 12;

uint32_t ReadU32(std::span<const uint8_t> data, size_t offset)
{
    return (uint32_t{data[offset]} << 24) | (uint32_t{data[offset + 1]} << 16)
           | (uint32_t{data[offset + 2]} << 8) | uint32_t{data[offset + 3]};
}

uint16_t ReadU16(std::span<const uint8_t> data, size_t offset)
{
    return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

double ReadS15Fixed16(std::span<const uint8_t> data, size_t offset)
{
    return static_cast<int32_t>(ReadU32(data, offset)) / 65536.0;
}

bool HasSignature(std::span<const uint8_t> data, size_t offset, const char (&signature)[5])
{
    return offset + 4 <= data.size() && std::memcmp(data.data() + offset, signature, 4) == 0;
}

// The tag's data, or an empty span if the tag is absent or out of bounds.
std::span<const uint8_t> FindTag(std::span<const uint8_t> icc, const char (&signature)[5])
{
    const uint32_t count = ReadU32(icc, kHeaderSize);
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t entry = kHeaderSize + 4 + size_t{i} * kTagEntrySize;
        if (entry + kTagEntrySize > icc.size())
        {
            break;
        }
        if (HasSignature(icc, entry, signature))
        {
            const size_t offset = ReadU32(icc, entry + 4);
            const size_t size = ReadU32(icc, entry + 8);
            if (offset > icc.size() || size > icc.size() - offset)
            {
                return {};
            }
            return icc.subspan(offset, size);
        }
    }
    return {};
}

std::optional<std::array<double, 3>> ReadXyzTag(std::span<const uint8_t> icc,
                                                const char (&signature)[5])
{
    const auto tag = FindTag(icc, signature);
    if (tag.size() < 20 || !HasSignature(tag, 0, "XYZ "))
    {
        return std::nullopt;
    }
    return std::array<double, 3>{ReadS15Fixed16(tag, 8), ReadS15Fixed16(tag, 12),
                                 ReadS15Fixed16(tag, 16)};
}

// True for a parametric curve of type 3 with the sRGB constants.
double SrgbToLinear(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// True for the sRGB tone curve, written either as a parametric curve of
// type 3 with the sRGB constants (v4 profiles such as Display P3) or as a
// sampled table (v2 profiles such as Windows' own sRGB, 1024 entries).
bool IsSrgbCurve(std::span<const uint8_t> tag)
{
    if (tag.size() >= 32 && HasSignature(tag, 0, "para") && ReadU16(tag, 8) == 3)
    {
        constexpr double kSrgb[] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
        for (size_t i = 0; i < 5; ++i)
        {
            if (std::abs(ReadS15Fixed16(tag, 12 + i * 4) - kSrgb[i]) > 1e-3)
            {
                return false;
            }
        }
        return true;
    }
    if (tag.size() >= 12 && HasSignature(tag, 0, "curv"))
    {
        // curveType: count, then that many uint16 samples over 0..1. Zero
        // or one entry means identity or a plain gamma - not sRGB.
        const size_t count = ReadU32(tag, 8);
        if (count < 2 || count > (tag.size() - 12) / 2)
        {
            return false;
        }
        for (size_t i = 0; i < count; ++i)
        {
            const double input = static_cast<double>(i) / static_cast<double>(count - 1);
            const double output = ReadU16(tag, 12 + i * 2) / 65535.0;
            // Well within one 8-bit step of the true curve.
            if (std::abs(output - SrgbToLinear(input)) > 1.0 / 1024.0)
            {
                return false;
            }
        }
        return true;
    }
    return false;
}

Matrix Multiply(const Matrix& a, const Matrix& b)
{
    Matrix result{};
    for (size_t row = 0; row < 3; ++row)
    {
        for (size_t col = 0; col < 3; ++col)
        {
            for (size_t k = 0; k < 3; ++k)
            {
                result[row * 3 + col] += a[row * 3 + k] * b[k * 3 + col];
            }
        }
    }
    return result;
}

}  // namespace

std::optional<std::array<float, 9>> SrgbCurveProfileToSrgbMatrix(std::span<const uint8_t> icc)
{
    if (icc.size() < kHeaderSize + 4 || !HasSignature(icc, 16, "RGB ")
        || !HasSignature(icc, 20, "XYZ "))
    {
        return std::nullopt;
    }
    if (!IsSrgbCurve(FindTag(icc, "rTRC")) || !IsSrgbCurve(FindTag(icc, "gTRC"))
        || !IsSrgbCurve(FindTag(icc, "bTRC")))
    {
        return std::nullopt;
    }
    const auto red = ReadXyzTag(icc, "rXYZ");
    const auto green = ReadXyzTag(icc, "gXYZ");
    const auto blue = ReadXyzTag(icc, "bXYZ");
    if (!red || !green || !blue)
    {
        return std::nullopt;
    }

    // Colorants are XYZ relative to the D50 PCS; columns are R, G, B.
    const Matrix toXyzD50 = {
        (*red)[0], (*green)[0], (*blue)[0],  //
        (*red)[1], (*green)[1], (*blue)[1],  //
        (*red)[2], (*green)[2], (*blue)[2],  //
    };
    // Bradford adaptation D50 -> D65, then XYZ (D65) -> linear sRGB.
    constexpr Matrix kD50ToD65 = {
        0.9555766, -0.0230393, 0.0631636,   //
        -0.0282895, 1.0099416, 0.0210077,   //
        0.0122982, -0.0204830, 1.3299098,   //
    };
    constexpr Matrix kXyzToSrgb = {
        3.2404542, -1.5371385, -0.4985314,  //
        -0.9692660, 1.8760108, 0.0415560,   //
        0.0556434, -0.2040259, 1.0572252,   //
    };
    const Matrix result = Multiply(kXyzToSrgb, Multiply(kD50ToD65, toXyzD50));

    std::array<float, 9> out{};
    for (size_t i = 0; i < 9; ++i)
    {
        out[i] = static_cast<float>(result[i]);
    }
    return out;
}

namespace
{

constexpr size_t kMinProfileSize = kHeaderSize + 4;  // header + tag count

std::wstring DescriptionFromText(std::span<const uint8_t> tag)
{
    // textDescriptionType (v2): 'desc', reserved, ASCII count, ASCII.
    const size_t count = ReadU32(tag, 8);
    if (count > tag.size() - 12)
    {
        return {};
    }
    std::wstring text;
    for (size_t i = 0; i < count && tag[12 + i] != 0; ++i)
    {
        text.push_back(static_cast<wchar_t>(tag[12 + i]));
    }
    return text;
}

std::wstring DescriptionFromMultiLocalized(std::span<const uint8_t> tag)
{
    // multiLocalizedUnicodeType (v4): 'mluc', reserved, record count,
    // record size, then (language, country, length, offset) records whose
    // strings are UTF-16BE at offsets from the tag start.
    const size_t records = ReadU32(tag, 8);
    const size_t recordSize = ReadU32(tag, 12);
    if (recordSize < 12 || records == 0 || records > (tag.size() - 16) / recordSize)
    {
        return {};
    }
    size_t chosen = 0;
    for (size_t i = 0; i < records; ++i)
    {
        const size_t record = 16 + i * recordSize;
        if (HasSignature(tag, record, "enUS"))
        {
            chosen = i;
            break;
        }
    }
    const size_t record = 16 + chosen * recordSize;
    const size_t length = ReadU32(tag, record + 4);
    const size_t offset = ReadU32(tag, record + 8);
    if (offset > tag.size() || length > tag.size() - offset)
    {
        return {};
    }
    std::wstring text;
    for (size_t i = 0; i + 1 < length; i += 2)
    {
        const wchar_t ch = static_cast<wchar_t>((tag[offset + i] << 8) | tag[offset + i + 1]);
        if (ch == 0)
        {
            break;
        }
        text.push_back(ch);
    }
    return text;
}

}  // namespace

bool IsRgbProfile(std::span<const uint8_t> icc)
{
    return icc.size() >= kMinProfileSize && HasSignature(icc, 36, "acsp")
           && HasSignature(icc, 16, "RGB ");
}

bool IsSrgbEquivalent(std::span<const uint8_t> icc)
{
    const auto matrix = SrgbCurveProfileToSrgbMatrix(icc);
    if (!matrix)
    {
        return false;
    }
    constexpr float kIdentity[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (size_t i = 0; i < 9; ++i)
    {
        // Colorants are stored as s15Fixed16 and rounded by whoever wrote
        // the profile; sRGB profiles land within a few thousandths.
        if (std::abs((*matrix)[i] - kIdentity[i]) > 5e-3f)
        {
            return false;
        }
    }
    return true;
}

std::wstring IccDescription(std::span<const uint8_t> icc)
{
    if (icc.size() < kMinProfileSize)
    {
        return {};
    }
    const auto tag = FindTag(icc, "desc");
    if (tag.size() < 16)
    {
        return {};
    }
    if (HasSignature(tag, 0, "desc"))
    {
        return DescriptionFromText(tag);
    }
    if (HasSignature(tag, 0, "mluc"))
    {
        return DescriptionFromMultiLocalized(tag);
    }
    return {};
}

std::vector<uint8_t> ReadJpegIccProfile(std::span<const uint8_t> jpeg)
{
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
    {
        return {};
    }
    // "ICC_PROFILE\0", then the 1-based chunk number and the chunk count.
    constexpr uint8_t kSignature[] = {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0};
    constexpr size_t kChunkHeader = sizeof(kSignature) + 2;

    std::vector<std::span<const uint8_t>> chunks;
    size_t expected = 0;
    size_t pos = 2;
    while (pos + 4 <= jpeg.size())
    {
        if (jpeg[pos] != 0xFF)
        {
            return {};
        }
        const uint8_t marker = jpeg[pos + 1];
        if (marker == 0xFF)
        {
            ++pos;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
        {
            pos += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9)
        {
            break;  // metadata comes before the scan
        }
        const size_t length = (size_t{jpeg[pos + 2]} << 8) | jpeg[pos + 3];
        if (length < 2 || length > jpeg.size() - pos - 2)
        {
            return {};
        }
        const auto payload = jpeg.subspan(pos + 4, length - 2);
        if (marker == 0xE2 && payload.size() > kChunkHeader
            && std::memcmp(payload.data(), kSignature, sizeof(kSignature)) == 0)
        {
            const size_t number = payload[sizeof(kSignature)];
            const size_t count = payload[sizeof(kSignature) + 1];
            if (count == 0 || number == 0 || number > count || (expected != 0 && count != expected))
            {
                return {};
            }
            expected = count;
            chunks.resize(count);
            if (!chunks[number - 1].empty())
            {
                return {};  // the same chunk twice
            }
            chunks[number - 1] = payload.subspan(kChunkHeader);
        }
        pos += 2 + length;
    }

    std::vector<uint8_t> profile;
    for (const auto& chunk : chunks)
    {
        if (chunk.empty())
        {
            return {};  // a chunk is missing
        }
        profile.insert(profile.end(), chunk.begin(), chunk.end());
    }
    return profile;
}
