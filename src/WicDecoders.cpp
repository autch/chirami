#include "WicDecoders.h"

#include <algorithm>
#include <cwctype>

namespace
{

std::wstring ToLower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text;
}

}  // namespace

std::unordered_set<std::wstring> QueryWicDecoderExtensions(IWICImagingFactory* factory)
{
    std::unordered_set<std::wstring> extensions;

    wil::com_ptr<IEnumUnknown> enumerator;
    THROW_IF_FAILED(factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault,
                                                       enumerator.put()));
    for (;;)
    {
        wil::com_ptr<IUnknown> unknown;
        ULONG fetched = 0;
        if (enumerator->Next(1, unknown.put(), &fetched) != S_OK)
        {
            break;
        }
        auto info = unknown.try_query<IWICBitmapDecoderInfo>();
        if (!info)
        {
            continue;
        }

        UINT length = 0;  // includes the terminating null
        if (FAILED(info->GetFileExtensions(0, nullptr, &length)) || length == 0)
        {
            continue;
        }
        std::wstring list(length, L'\0');
        if (FAILED(info->GetFileExtensions(length, list.data(), &length)))
        {
            continue;
        }
        list.resize(wcslen(list.c_str()));

        // Comma-separated, e.g. ".jpeg,.jpg,.jfif"
        size_t start = 0;
        while (start < list.size())
        {
            const size_t comma = list.find(L',', start);
            const size_t end = (comma == std::wstring::npos) ? list.size() : comma;
            if (end > start)
            {
                extensions.insert(ToLower(list.substr(start, end - start)));
            }
            if (comma == std::wstring::npos)
            {
                break;
            }
            start = comma + 1;
        }
    }
    return extensions;
}
