#include "MainWindow.h"

int MainWindow::OnCreate(LPCREATESTRUCT /*createStruct*/)
{
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put()));
    m_wicFactory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);
    return 0;
}

void MainWindow::OnSize(UINT /*type*/, CSize size)
{
    if (m_renderTarget)
    {
        m_renderTarget->Resize(D2D1::SizeU(size.cx, size.cy));
    }
}

void MainWindow::OnPaint(CDCHandle /*dc*/)
{
    // Direct2D presents by itself; no BeginPaint/EndPaint, but the update
    // region must still be validated or WM_PAINT will be sent forever.
    Render();
    ValidateRect(nullptr);
}

void MainWindow::OnDestroy()
{
    DiscardDeviceResources();
    PostQuitMessage(0);
}

HRESULT MainWindow::CreateDeviceResources()
{
    if (m_renderTarget)
    {
        return S_OK;
    }

    CRect rc;
    GetClientRect(&rc);

    return m_d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            m_hWnd, D2D1::SizeU(rc.Width(), rc.Height())),
        m_renderTarget.put());
}

void MainWindow::DiscardDeviceResources()
{
    m_renderTarget.reset();
}

void MainWindow::Render()
{
    if (FAILED(CreateDeviceResources()))
    {
        return;
    }

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));

    HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        DiscardDeviceResources();
        Invalidate(FALSE);
    }
}
