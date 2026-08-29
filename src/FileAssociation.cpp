#include "FileAssociation.h"
#include "StringUtil.h"
#include "WicDecoders.h"

#include <shellapi.h>  // ShellExecuteW
#include <shlobj.h>    // SHGetKnownFolderPath, SHChangeNotify

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <vector>

namespace FileAssociation
{
namespace
{

constexpr PCWSTR kAppName = L"chirami";
constexpr PCWSTR kAppRootKey = L"Software\\chirami";
constexpr PCWSTR kCapabilitiesKey = L"Software\\chirami\\Capabilities";
constexpr PCWSTR kRegisteredAppsKey = L"Software\\RegisteredApplications";
constexpr PCWSTR kClassesKey = L"Software\\Classes";
constexpr PCWSTR kProgIdPrefix = L"chirami.AssocFile.";
constexpr PCWSTR kFileExtsKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts";
constexpr PCWSTR kRegisteredExeValue = L"RegisteredExePath";
constexpr PCWSTR kUnregisterFileName = L"chirami-unregister.reg";

// IThumbnailProvider shell extension category, pointed at the CLSID of the
// OS photo thumbnail provider. No COM server of our own, hence no regsvr32;
// this is what makes OneDrive cloud-only placeholders show thumbnails.
constexpr PCWSTR kThumbnailCategory = L"{E357FCCD-A995-4576-B01F-234630154E96}";
constexpr PCWSTR kPhotoThumbnailProvider = L"{C7657C4A-9F68-40fa-A4DF-96BC08EB3551}";

HRESULT SetString(const std::wstring& subkey, PCWSTR name, const std::wstring& data)
{
    // RegSetKeyValueW creates intermediate keys as needed.
    return HRESULT_FROM_WIN32(
        RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), name, REG_SZ, data.c_str(),
                        static_cast<DWORD>((data.size() + 1) * sizeof(WCHAR))));
}

std::optional<std::wstring> GetString(PCWSTR subkey, PCWSTR name)
{
    WCHAR buffer[1024];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, name, RRF_RT_REG_SZ, nullptr, buffer, &size)
        != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return std::wstring(buffer);
}

// ".jpg" -> "JPG"
std::wstring ExtensionUpper(const std::wstring& extension)
{
    return ToUpper(extension.substr(1));
}

std::wstring ProgIdFor(const std::wstring& extension)
{
    return kProgIdPrefix + ExtensionUpper(extension);
}

// Sorted so the registry layout and the generated .reg file are stable.
std::vector<std::wstring> SortedExtensions(IWICImagingFactory* factory)
{
    std::vector<std::wstring> extensions;
    for (const std::wstring& extension : QueryWicDecoderExtensions(factory))
    {
        if (extension.size() >= 2 && extension.front() == L'.')
        {
            extensions.push_back(extension);
        }
    }
    std::sort(extensions.begin(), extensions.end());
    return extensions;
}

HRESULT WriteFileUtf16(const std::filesystem::path& path, const std::wstring& content) noexcept
try
{
    wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
    RETURN_LAST_ERROR_IF(!file);
    const WCHAR bom = 0xFEFF;
    DWORD written = 0;
    RETURN_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), &bom, sizeof(bom), &written, nullptr));
    RETURN_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(),
                                         static_cast<DWORD>(content.size() * sizeof(WCHAR)),
                                         &written, nullptr));
    return S_OK;
}
CATCH_RETURN()

// Regedit-importable fallback for when the app is deleted before
// unregistering. UserChoice removal here is unconditional (a .reg cannot
// check the current value); the in-app path is the careful one.
void WriteUnregisterRegFiles(const std::vector<std::wstring>& extensions)
{
    std::wstring content = L"Windows Registry Editor Version 5.00\r\n\r\n";
    content += L"; Removes the file associations registered by chirami (HKCU only).\r\n\r\n";
    for (const std::wstring& extension : extensions)
    {
        content += L"[-HKEY_CURRENT_USER\\Software\\Classes\\" + ProgIdFor(extension) + L"]\r\n";
    }
    content += L"\r\n[-HKEY_CURRENT_USER\\Software\\chirami]\r\n";
    content += L"\r\n[HKEY_CURRENT_USER\\Software\\RegisteredApplications]\r\n";
    content += L"\"chirami\"=-\r\n\r\n";
    for (const std::wstring& extension : extensions)
    {
        content += std::wstring(L"[-HKEY_CURRENT_USER\\") + kFileExtsKey + L"\\" + extension
                   + L"\\UserChoice]\r\n";
    }

    // Best effort on both copies: losing the fallback file must not fail the
    // registration that already happened.
    const std::filesystem::path exeDir =
        std::filesystem::path(CurrentExePath()).parent_path();
    LOG_IF_FAILED(WriteFileUtf16(exeDir / kUnregisterFileName, content));

    wil::unique_cotaskmem_string appData;
    if (SUCCEEDED(
            SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appData)))
    {
        const std::filesystem::path dir = std::filesystem::path(appData.get()) / kAppName;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        LOG_IF_FAILED(WriteFileUtf16(dir / kUnregisterFileName, content));
    }
}

std::vector<std::wstring> SubKeyNames(PCWSTR parent)
{
    std::vector<std::wstring> names;
    wil::unique_hkey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, parent, 0, KEY_READ, key.put()) != ERROR_SUCCESS)
    {
        return names;
    }
    for (DWORD index = 0;; ++index)
    {
        WCHAR name[256];
        DWORD length = ARRAYSIZE(name);
        const LSTATUS status =
            RegEnumKeyExW(key.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS)
        {
            break;
        }
        names.emplace_back(name, length);
    }
    return names;
}

bool StartsWithProgIdPrefix(const std::wstring& text)
{
    return _wcsnicmp(text.c_str(), kProgIdPrefix, wcslen(kProgIdPrefix)) == 0;
}

}  // namespace

Status Query()
{
    Status status;
    status.registered = GetString(kRegisteredAppsKey, kAppName).has_value();
    status.exePath = GetString(kCapabilitiesKey, kRegisteredExeValue).value_or(L"");
    return status;
}

std::wstring CurrentExePath()
{
    WCHAR buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, length);
}

HRESULT Register(IWICImagingFactory* factory, const std::wstring& appDescription,
                 const std::wstring& typeNameFormat) noexcept
try
{
    const std::wstring exe = CurrentExePath();
    const std::vector<std::wstring> extensions = SortedExtensions(factory);
    RETURN_HR_IF(E_UNEXPECTED, extensions.empty());

    // ProgID icon: the app icon embedded in the exe.
    const std::wstring defaultIcon = L"\"" + exe + L"\",0";

    for (const std::wstring& extension : extensions)
    {
        const std::wstring upper = ExtensionUpper(extension);
        const std::wstring base = std::wstring(kClassesKey) + L"\\" + ProgIdFor(extension);
        const std::wstring typeName =
            std::vformat(typeNameFormat, std::make_wformat_args(upper));

        RETURN_IF_FAILED(SetString(base, nullptr, typeName));
        RETURN_IF_FAILED(SetString(base, L"FriendlyTypeName", typeName));
        RETURN_IF_FAILED(SetString(base + L"\\DefaultIcon", nullptr, defaultIcon));
        RETURN_IF_FAILED(
            SetString(base + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\""));
        RETURN_IF_FAILED(SetString(base + L"\\shellex\\" + kThumbnailCategory, nullptr,
                                   kPhotoThumbnailProvider));
    }

    RETURN_IF_FAILED(SetString(kCapabilitiesKey, L"ApplicationName", kAppName));
    RETURN_IF_FAILED(SetString(kCapabilitiesKey, L"ApplicationDescription", appDescription));
    RETURN_IF_FAILED(SetString(kCapabilitiesKey, kRegisteredExeValue, exe));
    for (const std::wstring& extension : extensions)
    {
        RETURN_IF_FAILED(SetString(std::wstring(kCapabilitiesKey) + L"\\FileAssociations",
                                   extension.c_str(), ProgIdFor(extension)));
    }
    RETURN_IF_FAILED(SetString(kRegisteredAppsKey, kAppName, kCapabilitiesKey));

    WriteUnregisterRegFiles(extensions);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
CATCH_RETURN()

HRESULT Unregister() noexcept
try
{
    HRESULT result = S_OK;
    const auto keep = [&result](LSTATUS status) {
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && SUCCEEDED(result))
        {
            result = HRESULT_FROM_WIN32(status);  // remember, but keep cleaning
        }
    };

    for (const std::wstring& name : SubKeyNames(kClassesKey))
    {
        if (StartsWithProgIdPrefix(name))
        {
            keep(RegDeleteTreeW(HKEY_CURRENT_USER,
                                (std::wstring(kClassesKey) + L"\\" + name).c_str()));
        }
    }

    keep(RegDeleteTreeW(HKEY_CURRENT_USER, kAppRootKey));
    keep(RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegisteredAppsKey, kAppName));

    // Drop per-extension defaults only where they point at us; Explorer then
    // falls back to the machine-wide default (Photos etc.).
    for (const std::wstring& extension : SubKeyNames(kFileExtsKey))
    {
        const std::wstring userChoice =
            std::wstring(kFileExtsKey) + L"\\" + extension + L"\\UserChoice";
        const auto progId = GetString(userChoice.c_str(), L"ProgId");
        if (progId && StartsWithProgIdPrefix(*progId))
        {
            keep(RegDeleteTreeW(HKEY_CURRENT_USER, userChoice.c_str()));
        }
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return result;
}
CATCH_RETURN()

void OpenDefaultAppsSettings()
{
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=chirami",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace FileAssociation
