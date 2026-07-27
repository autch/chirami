#pragma once

#include "framework.h"

#include <string>

// Opt-in, per-user (HKCU only) file association, per DESIGN.md
// 「ファイルの関連付け」. Registers chirami as an association *candidate*
// for every extension an installed WIC decoder can open; promoting it to
// the default remains a user action in Windows Settings (UserChoice is
// hash-protected by design, and we do not work around that).
namespace FileAssociation
{

struct Status
{
    bool registered = false;  // RegisteredApplications entry present
    std::wstring exePath;     // exe path recorded at registration time
};

// Reads the registration state from HKCU. Registry only; fast.
Status Query();

// Full path of the running chirami.exe.
std::wstring CurrentExePath();

// Writes ProgIDs (with the OS photo thumbnail provider), Capabilities and
// the RegisteredApplications entry under HKCU, and generates
// chirami-unregister.reg next to the exe and under %APPDATA%\chirami.
// Running it again repairs a moved exe. typeNameFormat is a std::format
// pattern receiving the uppercase extension (e.g. L"chirami {} File").
HRESULT Register(IWICImagingFactory* factory, const std::wstring& appDescription,
                 const std::wstring& typeNameFormat) noexcept;

// Deletes everything Register() wrote. Per-extension UserChoice keys are
// removed only when they point at a chirami ProgID, so defaults chosen for
// other apps are left alone.
HRESULT Unregister() noexcept;

// Opens Settings > Apps > Default apps at chirami's page.
void OpenDefaultAppsSettings();

}  // namespace FileAssociation
