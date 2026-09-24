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
bool IsSrgbCurve(std::span<const uint8_t> tag)
{
    if (tag.size() < 32 || !HasSignature(tag, 0, "para") || ReadU16(tag, 8) != 3)
    {
        return false;
    }
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
