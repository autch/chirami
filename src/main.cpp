#include "framework.h"
#include "MainWindow.h"
#include "Settings.h"
#include "resource.h"

#include <shellapi.h>  // CommandLineToArgvW

CAppModule _Module;

namespace
{

void ApplyLanguage(const Settings& settings)
{
    if (settings.language == L"ja")
    {
        SetThreadUILanguage(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT));
    }
    else if (settings.language == L"en")
    {
        SetThreadUILanguage(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    }
    else
    {
        SetThreadUILanguage(GetUserDefaultUILanguage());
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR /*cmdLine*/, int cmdShow)
{
    Settings settings = Settings::Load();
    ApplyLanguage(settings);

    auto coInit = wil::CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    THROW_IF_FAILED(_Module.Init(nullptr, hInstance));
    auto moduleTerm = wil::scope_exit([] { _Module.Term(); });

    CMessageLoop msgLoop;
    _Module.AddMessageLoop(&msgLoop);
    auto removeLoop = wil::scope_exit([] { _Module.RemoveMessageLoop(); });

    WCHAR title[64];
    LoadStringW(hInstance, IDS_APP_TITLE, title, ARRAYSIZE(title));

    MainWindow wnd;
    wnd.InitSettings(std::move(settings));
    if (wnd.Create(nullptr, CWindow::rcDefault, title, WS_OVERLAPPEDWINDOW) == nullptr)
    {
        return 1;
    }
    wnd.ShowWindow(cmdShow);
    wnd.UpdateWindow();

    int argc = 0;
    wil::unique_hlocal_ptr<LPWSTR> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    if (argv && argc >= 2)
    {
        wnd.LoadFile(argv.get()[1]);
    }

    return msgLoop.Run();
}
