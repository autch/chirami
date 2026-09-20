#include "ProgIds.h"

#include "PathCompare.h"

namespace
{

constexpr std::wstring_view kProgIdPrefix = L"chirami.AssocFile.";

}  // namespace

std::wstring ExtensionLabel(std::wstring_view extension)
{
    if (!extension.empty() && extension.front() == L'.')
    {
        extension.remove_prefix(1);
    }
    return ToUpperInvariant(extension);
}

std::wstring ProgIdForExtension(std::wstring_view extension)
{
    return std::wstring(kProgIdPrefix) + ExtensionLabel(extension);
}

bool IsChiramiProgId(std::wstring_view name)
{
    return name.size() >= kProgIdPrefix.size()
           && EqualsNoCase(name.substr(0, kProgIdPrefix.size()), kProgIdPrefix);
}
