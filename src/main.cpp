#include "framework.h"
#include "MainWindow.h"
#include "resource.h"

CAppModule _Module;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR /*cmdLine*/, int cmdShow)
{
    SetThreadUILanguage(GetUserDefaultUILanguage());

    auto coInit = wil::CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    THROW_IF_FAILED(_Module.Init(nullptr, hInstance));
    auto moduleTerm = wil::scope_exit([] { _Module.Term(); });

    CMessageLoop msgLoop;
    _Module.AddMessageLoop(&msgLoop);
    auto removeLoop = wil::scope_exit([] { _Module.RemoveMessageLoop(); });

    WCHAR title[64];
    LoadStringW(hInstance, IDS_APP_TITLE, title, ARRAYSIZE(title));

    MainWindow wnd;
    if (wnd.Create(nullptr, CWindow::rcDefault, title, WS_OVERLAPPEDWINDOW) == nullptr)
    {
        return 1;
    }
    wnd.ShowWindow(cmdShow);
    wnd.UpdateWindow();

    return msgLoop.Run();
}
