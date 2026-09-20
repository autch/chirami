#include "WicDecoders.h"
#include "ImageFormatId.h"

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

        for (std::wstring& extension : SplitExtensionList(list))
        {
            extensions.insert(std::move(extension));
        }
    }
    return extensions;
}
