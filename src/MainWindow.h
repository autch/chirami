#pragma once

#include "framework.h"

class MainWindow : public CWindowImpl<MainWindow>
{
public:
    DECLARE_WND_CLASS_EX(L"ChiramiMainWindow", CS_HREDRAW | CS_VREDRAW, -1)

    BEGIN_MSG_MAP(MainWindow)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_SIZE(OnSize)
        MSG_WM_PAINT(OnPaint)
        MSG_WM_DESTROY(OnDestroy)
    END_MSG_MAP()

private:
    int OnCreate(LPCREATESTRUCT createStruct);
    void OnSize(UINT type, CSize size);
    void OnPaint(CDCHandle dc);
    void OnDestroy();

    HRESULT CreateDeviceResources();
    void DiscardDeviceResources();
    void Render();

    wil::com_ptr<ID2D1Factory> m_d2dFactory;
    wil::com_ptr<IWICImagingFactory> m_wicFactory;
    wil::com_ptr<ID2D1HwndRenderTarget> m_renderTarget;
};
