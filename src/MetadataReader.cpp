#include "MetadataReader.h"
#include "FileIo.h"
#include "StringUtil.h"
#include "resource.h"

#include <shlwapi.h>  // SHCreateMemStream, StrFormatByteSizeW

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>

namespace
{

// Sanity caps: a hostile file must not turn the properties window into a
// memory hog or an endless enumeration.
constexpr size_t kMaxValueChars = 256 * 1024;
constexpr size_t kMaxWicItems = 512;
constexpr int kMaxWicDepth = 8;
constexpr size_t kMaxVectorElements = 32;
constexpr size_t kMaxBlobBytes = 32;  // shown as hex before eliding

// UTF-8 first, then code page 1252. PNG tEXt is Latin-1 by spec, but AI
// tools (ComfyUI and friends) routinely write UTF-8 into it; EXIF ASCII
// fields get the same treatment. 1252 accepts any byte sequence, so this
// never fails.
std::wstring WidenBytes(const char* bytes, size_t length)
{
    if (length == 0 || bytes == nullptr)
    {
        return {};
    }
    length = std::min(length, kMaxValueChars * 3);
    const int inLength = static_cast<int>(length);
    UINT codePage = CP_UTF8;
    int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, inLength, nullptr, 0);
    if (wideLength <= 0)
    {
        codePage = 1252;
        wideLength = MultiByteToWideChar(codePage, 0, bytes, inLength, nullptr, 0);
        if (wideLength <= 0)
        {
            return {};
        }
    }
    std::wstring out(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(codePage, codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes,
                        inLength, out.data(), wideLength);
    while (!out.empty() && out.back() == L'\0')
    {
        out.pop_back();
    }
    return out;
}

std::wstring ClampValue(std::wstring value)
{
    if (value.size() > kMaxValueChars)
    {
        value.resize(kMaxValueChars);
        value += L" …";
    }
    return value;
}

std::wstring FormatGroupedNumber(uint64_t value)
{
    std::wstring digits = std::to_wstring(value);
    std::wstring out;
    out.reserve(digits.size() + digits.size() / 3);
    size_t remaining = digits.size();
    for (const wchar_t digit : digits)
    {
        out += digit;
        --remaining;
        if (remaining != 0 && remaining % 3 == 0)
        {
            out += L',';
        }
    }
    return out;
}

std::wstring FormatFileSize(uint64_t bytes)
{
    WCHAR pretty[64]{};
    StrFormatByteSizeW(static_cast<LONGLONG>(bytes), pretty, ARRAYSIZE(pretty));
    const std::wstring prettyText = pretty;
    if (bytes < 1024)
    {
        return prettyText;  // already exact; skip the redundant parenthesis
    }
    const std::wstring exactText = FormatGroupedNumber(bytes);
    return std::vformat(LoadStringResource(IDS_META_SIZE_FMT),
                        std::make_wformat_args(prettyText, exactText));
}

std::wstring FormatFileTime(const FILETIME& utc)
{
    SYSTEMTIME utcTime{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&utc, &utcTime)
        || !SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &local))
    {
        return {};
    }
    WCHAR date[80]{};
    WCHAR time[80]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date,
                    ARRAYSIZE(date), nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, nullptr, time, ARRAYSIZE(time));
    return std::wstring(date) + L" " + time;
}

// ---------------------------------------------------------------------------
// PNG text chunks (tEXt / iTXt / zTXt)
//
// Parsed directly from the file bytes because WIC has no iTXt reader at all,
// and AI generators store non-ASCII prompts (A1111 with Japanese text, for
// example) in iTXt. Compressed payloads would need zlib; they are rare, so
// they get a placeholder instead of a dependency.
// ---------------------------------------------------------------------------

uint32_t ReadBigEndian32(const uint8_t* bytes)
{
    return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8)
           | uint32_t{bytes[3]};
}

void ParsePngTextChunks(const uint8_t* data, size_t size, std::vector<MetadataItem>& items)
{
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < sizeof(kSignature) + 12
        || std::memcmp(data, kSignature, sizeof(kSignature)) != 0)
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
            value = payloadCompressed
                        ? compressedPlaceholder
                        : WidenBytes(chunk.data() + cursor, chunk.size() - cursor);
        }
        items.push_back({MetadataGroup::PngText, std::move(keyword), ClampValue(std::move(value))});
    }
}

// ---------------------------------------------------------------------------
// WIC metadata enumeration
// ---------------------------------------------------------------------------

// Friendly names for the EXIF/TIFF tags a viewer is likely to meet; unmapped
// tags keep their raw query path.
struct TagName
{
    uint16_t tag;
    PCWSTR name;
};

constexpr TagName kExifTagNames[] = {
    {256, L"ImageWidth"},
    {257, L"ImageHeight"},
    {258, L"BitsPerSample"},
    {259, L"Compression"},
    {270, L"ImageDescription"},
    {271, L"Make"},
    {272, L"Model"},
    {274, L"Orientation"},
    {282, L"XResolution"},
    {283, L"YResolution"},
    {296, L"ResolutionUnit"},
    {305, L"Software"},
    {306, L"DateTime"},
    {315, L"Artist"},
    {316, L"HostComputer"},
    {33432, L"Copyright"},
    {33434, L"ExposureTime"},
    {33437, L"FNumber"},
    {34850, L"ExposureProgram"},
    {34855, L"ISOSpeedRatings"},
    {36864, L"ExifVersion"},
    {36867, L"DateTimeOriginal"},
    {36868, L"DateTimeDigitized"},
    {37377, L"ShutterSpeedValue"},
    {37378, L"ApertureValue"},
    {37380, L"ExposureBiasValue"},
    {37381, L"MaxApertureValue"},
    {37383, L"MeteringMode"},
    {37384, L"LightSource"},
    {37385, L"Flash"},
    {37386, L"FocalLength"},
    {37510, L"UserComment"},
    {40961, L"ColorSpace"},
    {40962, L"PixelXDimension"},
    {40963, L"PixelYDimension"},
    {41986, L"ExposureMode"},
    {41987, L"WhiteBalance"},
    {41988, L"DigitalZoomRatio"},
    {41989, L"FocalLengthIn35mmFilm"},
    {41990, L"SceneCaptureType"},
    {42035, L"LensMake"},
    {42036, L"LensModel"},
};

constexpr TagName kGpsTagNames[] = {
    {0, L"GPSVersionID"},
    {1, L"GPSLatitudeRef"},
    {2, L"GPSLatitude"},
    {3, L"GPSLongitudeRef"},
    {4, L"GPSLongitude"},
    {5, L"GPSAltitudeRef"},
    {6, L"GPSAltitude"},
    {7, L"GPSTimeStamp"},
    {29, L"GPSDateStamp"},
};

// Extracts N from a trailing "/{ushort=N}" query segment.
bool TryParseUshortTag(const std::wstring& path, uint16_t& tag)
{
    constexpr std::wstring_view kPrefix = L"/{ushort=";
    const size_t start = path.rfind(kPrefix);
    if (start == std::wstring::npos || path.back() != L'}')
    {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = start + kPrefix.size(); i + 1 < path.size(); ++i)
    {
        if (path[i] < L'0' || path[i] > L'9')
        {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(path[i] - L'0');
        if (value > 0xFFFF)
        {
            return false;
        }
    }
    tag = static_cast<uint16_t>(value);
    return true;
}

std::wstring DisplayNameForWicPath(const std::wstring& path)
{
    uint16_t tag = 0;
    if (TryParseUshortTag(path, tag))
    {
        const bool gps = path.find(L"/gps") != std::wstring::npos;
        const auto* begin = gps ? std::begin(kGpsTagNames) : std::begin(kExifTagNames);
        const auto* end = gps ? std::end(kGpsTagNames) : std::end(kExifTagNames);
        for (const auto* entry = begin; entry != end; ++entry)
        {
            if (entry->tag == tag)
            {
                return entry->name;
            }
        }
    }
    return path;
}

// EXIF UserComment: an 8-byte character-set id followed by the text. JPEG AI
// generators put the prompt here, typically as UNICODE (UTF-16LE on
// Windows-written files).
bool DecodeExifUserComment(const uint8_t* bytes, size_t length, std::wstring& out)
{
    if (length < 8)
    {
        return false;
    }
    if (std::memcmp(bytes, "UNICODE\0", 8) == 0)
    {
        const size_t chars = (length - 8) / sizeof(wchar_t);
        out.assign(reinterpret_cast<const wchar_t*>(bytes + 8), chars);
        while (!out.empty() && out.back() == L'\0')
        {
            out.pop_back();
        }
        return true;
    }
    if (std::memcmp(bytes, "ASCII\0\0\0", 8) == 0
        || std::memcmp(bytes, "\0\0\0\0\0\0\0\0", 8) == 0)
    {
        out = WidenBytes(reinterpret_cast<const char*>(bytes) + 8, length - 8);
        return true;
    }
    return false;
}

bool AllPrintableAscii(const uint8_t* bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        if (bytes[i] < 0x20 || bytes[i] > 0x7E)
        {
            return false;
        }
    }
    return length > 0;
}

std::wstring FormatByteRun(const uint8_t* bytes, size_t length)
{
    // Short printable runs (ExifVersion and friends) read better as text.
    if (length <= 16 && AllPrintableAscii(bytes, length))
    {
        return WidenBytes(reinterpret_cast<const char*>(bytes), length);
    }
    std::wstring out;
    const size_t shown = std::min(length, kMaxBlobBytes);
    for (size_t i = 0; i < shown; ++i)
    {
        out += std::format(L"{}{:02X}", i == 0 ? L"" : L" ", bytes[i]);
    }
    if (shown < length)
    {
        out += std::format(L" … ({})", FormatGroupedNumber(length));
    }
    return out;
}

// EXIF rationals arrive packed into 64-bit values: low half numerator, high
// half denominator. A plain (unsigned) 64-bit number has a zero high half.
std::wstring FormatUnsigned64(ULONGLONG value)
{
    const uint32_t numerator = static_cast<uint32_t>(value & 0xFFFFFFFFull);
    const uint32_t denominator = static_cast<uint32_t>(value >> 32);
    if (denominator == 0)
    {
        return std::to_wstring(numerator);
    }
    return std::format(L"{}/{}", numerator, denominator);
}

std::wstring FormatSigned64(LONGLONG value)
{
    const int32_t numerator = static_cast<int32_t>(value & 0xFFFFFFFFll);
    const int32_t denominator = static_cast<int32_t>(static_cast<uint64_t>(value) >> 32);
    if (denominator == 0)
    {
        return std::to_wstring(value);
    }
    return std::format(L"{}/{}", numerator, denominator);
}

template <typename T, typename Formatter>
std::wstring JoinVector(const T* values, ULONG count, Formatter&& format)
{
    std::wstring out;
    const ULONG shown = static_cast<ULONG>(std::min<size_t>(count, kMaxVectorElements));
    for (ULONG i = 0; i < shown; ++i)
    {
        if (i != 0)
        {
            out += L", ";
        }
        out += format(values[i]);
    }
    if (shown < count)
    {
        out += std::format(L", … ({})", count);
    }
    return out;
}

// Formats a leaf PROPVARIANT for display. Returns false for types that have
// no useful text form (those rows are skipped).
bool FormatPropVariant(const PROPVARIANT& value, const std::wstring& path, std::wstring& out)
{
    // The prompt in AI-generated JPEGs commonly hides in UserComment's raw
    // bytes; decode its character-set prefix instead of hex-dumping it.
    uint16_t tag = 0;
    if (TryParseUshortTag(path, tag) && tag == 37510)
    {
        const uint8_t* bytes = nullptr;
        size_t length = 0;
        if (value.vt == (VT_VECTOR | VT_UI1))
        {
            bytes = value.caub.pElems;
            length = value.caub.cElems;
        }
        else if (value.vt == VT_BLOB)
        {
            bytes = value.blob.pBlobData;
            length = value.blob.cbSize;
        }
        if (bytes != nullptr && DecodeExifUserComment(bytes, length, out))
        {
            return true;
        }
    }

    switch (value.vt)
    {
    case VT_LPWSTR:
        out = value.pwszVal != nullptr ? value.pwszVal : L"";
        return true;
    case VT_BSTR:
        out = value.bstrVal != nullptr ? value.bstrVal : L"";
        return true;
    case VT_LPSTR:
        out = value.pszVal != nullptr
                  ? WidenBytes(value.pszVal, std::strlen(value.pszVal))
                  : L"";
        return true;
    case VT_BOOL:
        out = value.boolVal != VARIANT_FALSE ? L"true" : L"false";
        return true;
    case VT_UI1:
        out = std::to_wstring(value.bVal);
        return true;
    case VT_UI2:
        out = std::to_wstring(value.uiVal);
        return true;
    case VT_UI4:
        out = std::to_wstring(value.ulVal);
        return true;
    case VT_I1:
        out = std::to_wstring(value.cVal);
        return true;
    case VT_I2:
        out = std::to_wstring(value.iVal);
        return true;
    case VT_I4:
        out = std::to_wstring(value.lVal);
        return true;
    case VT_UI8:
        out = FormatUnsigned64(value.uhVal.QuadPart);
        return true;
    case VT_I8:
        out = FormatSigned64(value.hVal.QuadPart);
        return true;
    case VT_R4:
        out = std::format(L"{:g}", value.fltVal);
        return true;
    case VT_R8:
        out = std::format(L"{:g}", value.dblVal);
        return true;
    case VT_FILETIME:
        out = FormatFileTime(value.filetime);
        return true;
    case VT_CLSID:
    {
        if (value.puuid == nullptr)
        {
            return false;
        }
        WCHAR guid[64]{};
        StringFromGUID2(*value.puuid, guid, ARRAYSIZE(guid));
        out = guid;
        return true;
    }
    case VT_BLOB:
        out = FormatByteRun(value.blob.pBlobData, value.blob.cbSize);
        return true;
    case VT_VECTOR | VT_UI1:
        out = FormatByteRun(value.caub.pElems, value.caub.cElems);
        return true;
    case VT_VECTOR | VT_UI2:
        out = JoinVector(value.caui.pElems, value.caui.cElems,
                         [](USHORT v) { return std::to_wstring(v); });
        return true;
    case VT_VECTOR | VT_UI4:
        out = JoinVector(value.caul.pElems, value.caul.cElems,
                         [](ULONG v) { return std::to_wstring(v); });
        return true;
    case VT_VECTOR | VT_I2:
        out = JoinVector(value.cai.pElems, value.cai.cElems,
                         [](SHORT v) { return std::to_wstring(v); });
        return true;
    case VT_VECTOR | VT_I4:
        out = JoinVector(value.cal.pElems, value.cal.cElems,
                         [](LONG v) { return std::to_wstring(v); });
        return true;
    case VT_VECTOR | VT_UI8:
        out = JoinVector(value.cauh.pElems, value.cauh.cElems,
                         [](const ULARGE_INTEGER& v) { return FormatUnsigned64(v.QuadPart); });
        return true;
    case VT_VECTOR | VT_I8:
        out = JoinVector(value.cah.pElems, value.cah.cElems,
                         [](const LARGE_INTEGER& v) { return FormatSigned64(v.QuadPart); });
        return true;
    case VT_VECTOR | VT_R4:
        out = JoinVector(value.caflt.pElems, value.caflt.cElems,
                         [](float v) { return std::format(L"{:g}", v); });
        return true;
    case VT_VECTOR | VT_R8:
        out = JoinVector(value.cadbl.pElems, value.cadbl.cElems,
                         [](double v) { return std::format(L"{:g}", v); });
        return true;
    case VT_VECTOR | VT_LPSTR:
        out = JoinVector(value.calpstr.pElems, value.calpstr.cElems, [](LPSTR v) {
            return v != nullptr ? WidenBytes(v, std::strlen(v)) : std::wstring();
        });
        return true;
    case VT_VECTOR | VT_LPWSTR:
        out = JoinVector(value.calpwstr.pElems, value.calpwstr.cElems,
                         [](LPWSTR v) { return std::wstring(v != nullptr ? v : L""); });
        return true;
    default:
        return false;  // no sensible text form
    }
}

// A raw PNG chunk blob (length + type + payload) whose type is one of the
// text chunks. WIC wraps chunks it has no reader for (zTXt, iTXt) in the
// "unknown" metadata handler as raw bytes; the dedicated parser already
// covers those, so their hex dumps are pure noise.
bool IsPngTextChunkBlob(const PROPVARIANT& value)
{
    const uint8_t* bytes = nullptr;
    size_t length = 0;
    if (value.vt == (VT_VECTOR | VT_UI1))
    {
        bytes = value.caub.pElems;
        length = value.caub.cElems;
    }
    else if (value.vt == VT_BLOB)
    {
        bytes = value.blob.pBlobData;
        length = value.blob.cbSize;
    }
    if (bytes == nullptr || length < 8)
    {
        return false;
    }
    return std::memcmp(bytes + 4, "tEXt", 4) == 0 || std::memcmp(bytes + 4, "iTXt", 4) == 0
           || std::memcmp(bytes + 4, "zTXt", 4) == 0;
}

// Recursively walks a metadata query reader, appending one row per leaf
// value. `skipPngText` filters PNG text chunks that the dedicated parser
// already produced (in full, including iTXt).
void EnumerateWicMetadata(IWICMetadataQueryReader* reader, const std::wstring& prefix, int depth,
                          bool skipPngText, std::vector<MetadataItem>& items)
{
    if (reader == nullptr || depth > kMaxWicDepth)
    {
        return;
    }
    wil::com_ptr<IEnumString> names;
    if (FAILED(reader->GetEnumerator(names.put())))
    {
        return;
    }
    for (;;)
    {
        if (items.size() >= kMaxWicItems)
        {
            return;
        }
        wil::unique_cotaskmem_string name;
        ULONG fetched = 0;
        if (names->Next(1, name.put(), &fetched) != S_OK || fetched == 0)
        {
            break;
        }
        const std::wstring path = prefix + name.get();
        // Duplicate chunks enumerate as "/[1]tEXt" etc., so match by
        // substring, not equality.
        if (skipPngText && depth == 0
            && (path.find(L"tEXt") != std::wstring::npos
                || path.find(L"iTXt") != std::wstring::npos
                || path.find(L"zTXt") != std::wstring::npos))
        {
            continue;
        }

        wil::unique_prop_variant value;
        if (FAILED(reader->GetMetadataByName(name.get(), &value)))
        {
            continue;
        }
        if (skipPngText && IsPngTextChunkBlob(value))
        {
            continue;
        }
        if (value.vt == VT_UNKNOWN && value.punkVal != nullptr)
        {
            wil::com_ptr<IWICMetadataQueryReader> nested;
            if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(nested.put()))))
            {
                EnumerateWicMetadata(nested.get(), path, depth + 1, skipPngText, items);
            }
            continue;
        }
        std::wstring text;
        if (FormatPropVariant(value, path, text))
        {
            items.push_back({MetadataGroup::Wic, DisplayNameForWicPath(path),
                             ClampValue(std::move(text))});
        }
    }
}

std::wstring ComponentFriendlyName(IWICComponentInfo* info)
{
    if (info == nullptr)
    {
        return {};
    }
    UINT length = 0;
    if (FAILED(info->GetFriendlyName(0, nullptr, &length)) || length == 0)
    {
        return {};
    }
    std::wstring name(length, L'\0');
    if (FAILED(info->GetFriendlyName(length, name.data(), &length)))
    {
        return {};
    }
    while (!name.empty() && name.back() == L'\0')
    {
        name.pop_back();
    }
    return name;
}

}  // namespace

MetadataReader::MetadataReader(HWND notifyWindow, UINT notifyMessage, LANGID language)
    : m_notifyWindow(notifyWindow),
      m_notifyMessage(notifyMessage),
      m_language(language),
      m_thread([this](std::stop_token stopToken) { WorkerProc(stopToken); })
{
}

uint64_t MetadataReader::RequestRead(std::filesystem::path path)
{
    uint64_t generation;
    {
        std::lock_guard lock(m_mutex);
        generation = m_nextGeneration++;
        m_pending = Request{generation, std::move(path)};
    }
    m_cv.notify_one();
    return generation;
}

std::optional<MetadataReader::Result> MetadataReader::TakeResult()
{
    std::lock_guard lock(m_mutex);
    return std::exchange(m_completed, std::nullopt);
}

bool MetadataReader::ShouldAbort(const std::stop_token& stopToken)
{
    if (stopToken.stop_requested())
    {
        return true;
    }
    std::lock_guard lock(m_mutex);
    return m_pending.has_value();  // a newer request supersedes this one
}

void MetadataReader::WorkerProc(std::stop_token stopToken) noexcept
try
{
    // Item names load from the string table on this thread; match the UI
    // language chosen at startup.
    SetThreadUILanguage(m_language);

    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    auto factory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);

    while (true)
    {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, stopToken, [this] { return m_pending.has_value(); }))
            {
                return;  // stop requested
            }
            request = std::move(*m_pending);
            m_pending.reset();
        }

        Result result;
        result.generation = request.generation;
        result.path = request.path;
        result.hr = Read(factory.get(), request, stopToken, result.items);

        if (stopToken.stop_requested())
        {
            return;
        }
        if (result.hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            continue;  // superseded by a newer request; skip notification
        }

        const uint64_t generation = result.generation;
        {
            std::lock_guard lock(m_mutex);
            m_completed = std::move(result);
        }
        PostMessageW(m_notifyWindow, m_notifyMessage, 0, static_cast<LPARAM>(generation));
    }
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
}

HRESULT MetadataReader::Read(IWICImagingFactory* factory, const Request& request,
                             const std::stop_token& stopToken,
                             std::vector<MetadataItem>& items) noexcept
try
{
    // --- File group -------------------------------------------------------
    wil::unique_hfile file(CreateFileW(request.path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    RETURN_LAST_ERROR_IF(!file);

    BY_HANDLE_FILE_INFORMATION info{};
    RETURN_IF_WIN32_BOOL_FALSE(GetFileInformationByHandle(file.get(), &info));
    const uint64_t fileSize = (uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;

    items.push_back({MetadataGroup::File, LoadStringResource(IDS_META_FILENAME),
                     request.path.filename().wstring()});
    items.push_back({MetadataGroup::File, LoadStringResource(IDS_META_FOLDER),
                     request.path.parent_path().wstring()});
    items.push_back(
        {MetadataGroup::File, LoadStringResource(IDS_META_FILESIZE), FormatFileSize(fileSize)});
    items.push_back({MetadataGroup::File, LoadStringResource(IDS_META_MODIFIED),
                     FormatFileTime(info.ftLastWriteTime)});
    items.push_back({MetadataGroup::File, LoadStringResource(IDS_META_CREATED),
                     FormatFileTime(info.ftCreationTime)});

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), fileSize > UINT_MAX);

    // Chunked so a stale request on a slow share does not wedge this worker.
    std::vector<uint8_t> data(static_cast<size_t>(fileSize));
    RETURN_IF_FAILED(ReadFileChunked(file.get(), data, [&] { return ShouldAbort(stopToken); }));

    // --- Image group ------------------------------------------------------
    wil::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(data.data(), static_cast<UINT>(data.size())));
    RETURN_HR_IF_NULL(E_OUTOFMEMORY, stream.get());

    wil::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, decoder.put())))
    {
        return S_OK;  // not a decodable image; the file group still shows
    }

    {
        wil::com_ptr<IWICBitmapDecoderInfo> decoderInfo;
        if (SUCCEEDED(decoder->GetDecoderInfo(decoderInfo.put())))
        {
            if (auto name = ComponentFriendlyName(decoderInfo.get()); !name.empty())
            {
                items.push_back(
                    {MetadataGroup::Image, LoadStringResource(IDS_META_FORMAT), std::move(name)});
            }
        }
    }

    wil::com_ptr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(decoder->GetFrame(0, frame.put())))
    {
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(frame->GetSize(&width, &height)))
        {
            items.push_back({MetadataGroup::Image, LoadStringResource(IDS_META_DIMENSIONS),
                             std::format(L"{} × {}", width, height)});
        }
        WICPixelFormatGUID pixelFormat{};
        if (SUCCEEDED(frame->GetPixelFormat(&pixelFormat)))
        {
            wil::com_ptr<IWICComponentInfo> componentInfo;
            if (SUCCEEDED(factory->CreateComponentInfo(pixelFormat, componentInfo.put())))
            {
                if (auto name = ComponentFriendlyName(componentInfo.get()); !name.empty())
                {
                    items.push_back({MetadataGroup::Image,
                                     LoadStringResource(IDS_META_PIXELFORMAT), std::move(name)});
                }
            }
        }
        double dpiX = 0.0;
        double dpiY = 0.0;
        if (SUCCEEDED(frame->GetResolution(&dpiX, &dpiY)))
        {
            items.push_back({MetadataGroup::Image, LoadStringResource(IDS_META_DPI),
                             std::format(L"{:g} × {:g}", dpiX, dpiY)});
        }
    }

    UINT frameCount = 1;
    if (SUCCEEDED(decoder->GetFrameCount(&frameCount)))
    {
        items.push_back({MetadataGroup::Image, LoadStringResource(IDS_META_FRAMES),
                         std::to_wstring(frameCount)});
    }

    if (ShouldAbort(stopToken))
    {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    // --- PNG text + generic WIC metadata ----------------------------------
    GUID container{};
    (void)decoder->GetContainerFormat(&container);
    const bool isPng = container == GUID_ContainerFormatPng;
    if (isPng)
    {
        ParsePngTextChunks(data.data(), data.size(), items);
    }

    if (frame)
    {
        wil::com_ptr<IWICMetadataQueryReader> reader;
        if (SUCCEEDED(frame->GetMetadataQueryReader(reader.put())))
        {
            EnumerateWicMetadata(reader.get(), L"", 0, isPng, items);
        }
    }
    {
        // Container-level metadata (GIF logical screen etc.).
        wil::com_ptr<IWICMetadataQueryReader> reader;
        if (SUCCEEDED(decoder->GetMetadataQueryReader(reader.put())))
        {
            EnumerateWicMetadata(reader.get(), L"", 0, isPng, items);
        }
    }
    return S_OK;
}
CATCH_RETURN()
