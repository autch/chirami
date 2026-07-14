#include "Settings.h"

#include "framework.h"

#include <shlobj.h>  // SHGetKnownFolderPath

namespace
{

constexpr const WCHAR kSection[] = L"chirami";

std::wstring ReadString(const std::filesystem::path& file, const WCHAR* key,
                        const WCHAR* defaultValue)
{
    WCHAR buffer[64];
    GetPrivateProfileStringW(kSection, key, defaultValue, buffer, ARRAYSIZE(buffer),
                             file.c_str());
    return buffer;
}

}  // namespace

std::filesystem::path Settings::FilePath()
{
    wil::unique_cotaskmem_string appData;
    THROW_IF_FAILED(
        SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appData));
    return std::filesystem::path(appData.get()) / L"chirami" / L"chirami.ini";
}

Settings Settings::Load()
try
{
    const auto file = FilePath();
    Settings settings;

    settings.language = ReadString(file, L"Language", L"");

    const std::wstring key = ReadString(file, L"SortKey", L"name");
    settings.sortKey = key == L"date" ? SortKey::Date
                       : key == L"size" ? SortKey::Size
                                        : SortKey::Name;
    settings.sortDescending = ReadString(file, L"SortDescending", L"0") == L"1";
    return settings;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return Settings{};  // fall back to defaults; viewing must not be blocked
}

void Settings::Save() const
try
{
    const auto file = FilePath();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    WritePrivateProfileStringW(kSection, L"Language", language.c_str(), file.c_str());
    const WCHAR* key = sortKey == SortKey::Date ? L"date"
                       : sortKey == SortKey::Size ? L"size"
                                                  : L"name";
    WritePrivateProfileStringW(kSection, L"SortKey", key, file.c_str());
    WritePrivateProfileStringW(kSection, L"SortDescending", sortDescending ? L"1" : L"0",
                               file.c_str());
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
}
