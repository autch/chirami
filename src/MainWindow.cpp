#include "MainWindow.h"
#include "resource.h"

#include <shellapi.h>  // DragAcceptFiles, DragQueryFileW

#include <algorithm>
#include <cmath>
#include <format>

namespace
{

constexpr float kMinZoom = 0.01f;
constexpr float kMaxZoom = 32.0f;
constexpr float kZoomStep = 1.25f;
constexpr float kScrollLine = 48.0f;    // 96-DPI pixels per scrollbar arrow
constexpr float kWheelScroll = 96.0f;   // 96-DPI pixels per wheel notch
constexpr float kStatusFontSize = 16.0f;  // 96-DPI pixels

std::wstring LoadStringResource(UINT id)
{
    WCHAR buffer[256];
    const int length = LoadStringW(_Module.GetResourceInstance(), id, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, length > 0 ? static_cast<size_t>(length) : 0);
}

UINT ErrorStringId(HRESULT hr)
{
    if (hr == kHrImageTooLarge)
    {
        return IDS_ERR_IMAGE_TOO_LARGE;
    }
    if (HRESULT_FACILITY(hr) == FACILITY_WINCODEC_DWRITE_DWM)
    {
        return IDS_ERR_DECODE;
    }
    return IDS_ERR_FILE_OPEN;
}

}  // namespace

int MainWindow::OnCreate(LPCREATESTRUCT /*createStruct*/)
{
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put()));
    m_wicFactory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);

    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(m_dwriteFactory.put())));
    m_dpi = GetDpiForWindow(m_hWnd);
    THROW_IF_FAILED(CreateTextFormat());

    m_loader = std::make_unique<ImageLoader>(m_hWnd, WM_APP_IMAGE_LOADED);
    m_scanner = std::make_unique<FolderScanner>(m_hWnd, WM_APP_FOLDER_SCANNED);

    DragAcceptFiles(TRUE);
    return 0;
}

HRESULT MainWindow::CreateTextFormat()
{
    // The render target is fixed at 96 DPI (1 DIP == 1 physical pixel), so
    // our own UI text has to scale with the window DPI explicitly.
    m_textFormat.reset();
    RETURN_IF_FAILED(m_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, kStatusFontSize * DpiScale(), L"", m_textFormat.put()));
    RETURN_IF_FAILED(m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
    RETURN_IF_FAILED(m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
    return S_OK;
}

LRESULT MainWindow::OnDpiChanged(UINT /*msg*/, WPARAM wParam, LPARAM lParam, BOOL& /*handled*/)
{
    m_dpi = LOWORD(wParam);
    LOG_IF_FAILED(CreateTextFormat());

    // Adopt the size the OS suggests for the new monitor; the resulting
    // WM_SIZE updates the render target and scrollbars.
    if (const RECT* suggested = reinterpret_cast<const RECT*>(lParam))
    {
        SetWindowPos(nullptr, suggested, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Invalidate(FALSE);
    return 0;
}

void MainWindow::LoadFile(std::filesystem::path path)
{
    // Purely lexical (cwd-based); safe even for paths on slow shares.
    std::error_code ec;
    if (auto absolute = std::filesystem::absolute(path, ec); !ec)
    {
        path = std::move(absolute);
    }

    m_currentPath = std::move(path);
    m_expectedGeneration = m_loader->RequestLoad(m_currentPath);
    m_state = ViewState::Loading;
    m_statusText = LoadStringResource(IDS_STATUS_LOADING);
    m_edgeWarned = false;

    if (auto folder = m_currentPath.parent_path(); folder != m_currentFolder)
    {
        m_currentFolder = std::move(folder);
        m_folderFiles.clear();
        m_currentIndex = -1;
        m_expectedScanGeneration = m_scanner->RequestScan(m_currentFolder);
    }
    else if (!m_folderFiles.empty())
    {
        m_currentIndex = FindFolderIndex(m_currentPath);
    }

    Invalidate(FALSE);
}

void MainWindow::NavigateBy(int delta)
{
    if (m_folderFiles.empty())
    {
        return;  // no scan result yet (or nothing decodable in the folder)
    }
    const ptrdiff_t last = static_cast<ptrdiff_t>(m_folderFiles.size()) - 1;
    ptrdiff_t target;
    if (m_currentIndex < 0)
    {
        target = delta > 0 ? 0 : last;
    }
    else
    {
        target = m_currentIndex + delta;
        if (target < 0 || target > last)
        {
            // Past the edge: beep once as a warning, wrap around to the
            // other end on the next attempt. (Wrap behavior will become
            // configurable later.)
            if (!m_edgeWarned)
            {
                MessageBeep(MB_OK);
                m_edgeWarned = true;
                return;
            }
            target = target < 0 ? last : 0;
        }
    }
    if (target == m_currentIndex)
    {
        return;  // e.g. wrapping in a single-file folder
    }
    m_currentIndex = target;
    LoadFile(m_folderFiles[static_cast<size_t>(target)]);
}

ptrdiff_t MainWindow::FindFolderIndex(const std::filesystem::path& path) const
{
    for (size_t i = 0; i < m_folderFiles.size(); ++i)
    {
        if (_wcsicmp(m_folderFiles[i].c_str(), path.c_str()) == 0)
        {
            return static_cast<ptrdiff_t>(i);
        }
    }
    return -1;
}

void MainWindow::OnKeyDown(UINT key, UINT /*repeatCount*/, UINT /*flags*/)
{
    CRect rc;
    GetClientRect(&rc);
    const CPoint center(rc.Width() / 2, rc.Height() / 2);

    switch (key)
    {
    case VK_LEFT:
        NavigateBy(-1);
        break;
    case VK_RIGHT:
        NavigateBy(+1);
        break;
    case VK_OEM_PLUS:
    case VK_ADD:
        StepZoom(+1, center);
        break;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        StepZoom(-1, center);
        break;
    case '1':
    case VK_NUMPAD1:
        ToggleActualSize();
        break;
    default:
        break;
    }
}

MainWindow::ViewLayout MainWindow::ComputeLayout()
{
    ViewLayout layout;
    if (!m_cpuImage)
    {
        return layout;
    }

    CRect rc;
    GetClientRect(&rc);
    const float clientWidth = static_cast<float>(rc.Width());
    const float clientHeight = static_cast<float>(rc.Height());
    const float imageWidth = static_cast<float>(m_cpuImage.width);
    const float imageHeight = static_cast<float>(m_cpuImage.height);

    switch (m_zoomMode)
    {
    case ZoomMode::Fit:
        // Shrink to fit while keeping the aspect ratio, never upscale.
        layout.scale = std::min(
            {1.0f, clientWidth / imageWidth, clientHeight / imageHeight});
        break;
    case ZoomMode::ActualSize:
        layout.scale = 1.0f;
        break;
    case ZoomMode::Custom:
        layout.scale = m_zoomScale;
        break;
    }

    layout.displayWidth = imageWidth * layout.scale;
    layout.displayHeight = imageHeight * layout.scale;
    layout.maxPanX = std::max(0.0f, layout.displayWidth - clientWidth);
    layout.maxPanY = std::max(0.0f, layout.displayHeight - clientHeight);

    m_panX = std::clamp(m_panX, 0.0f, layout.maxPanX);
    m_panY = std::clamp(m_panY, 0.0f, layout.maxPanY);

    // Integer pixel offsets keep 100% display exactly dot-by-dot.
    layout.destX = layout.maxPanX > 0.0f
                       ? -std::round(m_panX)
                       : std::round((clientWidth - layout.displayWidth) / 2.0f);
    layout.destY = layout.maxPanY > 0.0f
                       ? -std::round(m_panY)
                       : std::round((clientHeight - layout.displayHeight) / 2.0f);
    return layout;
}

void MainWindow::UpdateScrollBars()
{
    // ShowScrollBar changes the client size and re-enters via WM_SIZE.
    if (m_updatingScrollBars || !IsWindow())
    {
        return;
    }
    m_updatingScrollBars = true;
    auto guard = wil::scope_exit([&] { m_updatingScrollBars = false; });

    bool needH = false;
    bool needV = false;
    float displayWidth = 0.0f;
    float displayHeight = 0.0f;

    // Fit mode never overflows the client area, so bars stay hidden.
    if (m_cpuImage && m_zoomMode != ZoomMode::Fit)
    {
        const float scale = m_zoomMode == ZoomMode::ActualSize ? 1.0f : m_zoomScale;
        displayWidth = static_cast<float>(m_cpuImage.width) * scale;
        displayHeight = static_cast<float>(m_cpuImage.height) * scale;

        // Decide visibility against the bar-less ("gross") client size; each
        // bar that becomes visible steals space, which can force the other
        // one, hence the two passes.
        CRect rc;
        GetClientRect(&rc);
        const int barWidth = GetSystemMetrics(SM_CXVSCROLL);
        const int barHeight = GetSystemMetrics(SM_CYHSCROLL);
        const DWORD style = GetStyle();
        const int grossWidth = rc.Width() + ((style & WS_VSCROLL) ? barWidth : 0);
        const int grossHeight = rc.Height() + ((style & WS_HSCROLL) ? barHeight : 0);
        for (int pass = 0; pass < 2; ++pass)
        {
            const float availWidth = static_cast<float>(grossWidth - (needV ? barWidth : 0));
            const float availHeight = static_cast<float>(grossHeight - (needH ? barHeight : 0));
            needH = displayWidth > availWidth;
            needV = displayHeight > availHeight;
        }
    }

    const DWORD style = GetStyle();
    if (needH != ((style & WS_HSCROLL) != 0))
    {
        ShowScrollBar(SB_HORZ, needH);
    }
    if (needV != ((style & WS_VSCROLL) != 0))
    {
        ShowScrollBar(SB_VERT, needV);
    }

    CRect rc;
    GetClientRect(&rc);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (needH)
    {
        si.nMin = 0;
        si.nMax = static_cast<int>(displayWidth) - 1;
        si.nPage = rc.Width();
        si.nPos = static_cast<int>(m_panX);
        SetScrollInfo(SB_HORZ, &si, TRUE);
    }
    if (needV)
    {
        si.nMin = 0;
        si.nMax = static_cast<int>(displayHeight) - 1;
        si.nPage = rc.Height();
        si.nPos = static_cast<int>(m_panY);
        SetScrollInfo(SB_VERT, &si, TRUE);
    }
}

void MainWindow::SyncScrollPositions()
{
    const DWORD style = GetStyle();
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    if (style & WS_HSCROLL)
    {
        si.nPos = static_cast<int>(m_panX);
        SetScrollInfo(SB_HORZ, &si, TRUE);
    }
    if (style & WS_VSCROLL)
    {
        si.nPos = static_cast<int>(m_panY);
        SetScrollInfo(SB_VERT, &si, TRUE);
    }
}

void MainWindow::UpdateView()
{
    ComputeLayout();  // clamps pan
    SyncScrollPositions();
    Invalidate(FALSE);
}

void MainWindow::ApplyZoom(float newScale, CPoint anchor)
{
    newScale = std::clamp(newScale, kMinZoom, kMaxZoom);
    const ViewLayout layout = ComputeLayout();
    if (layout.scale <= 0.0f || newScale == layout.scale)
    {
        return;
    }

    // Keep the image point under `anchor` stationary on screen.
    const float imageX = (static_cast<float>(anchor.x) - layout.destX) / layout.scale;
    const float imageY = (static_cast<float>(anchor.y) - layout.destY) / layout.scale;

    m_zoomMode = ZoomMode::Custom;
    m_zoomScale = newScale;
    m_panX = imageX * newScale - static_cast<float>(anchor.x);
    m_panY = imageY * newScale - static_cast<float>(anchor.y);

    UpdateScrollBars();
    UpdateView();
}

void MainWindow::StepZoom(int direction, CPoint anchor)
{
    const ViewLayout layout = ComputeLayout();
    if (layout.scale <= 0.0f)
    {
        return;
    }
    ApplyZoom(direction > 0 ? layout.scale * kZoomStep : layout.scale / kZoomStep, anchor);
}

void MainWindow::ToggleActualSize()
{
    if (!m_cpuImage)
    {
        return;
    }
    m_zoomMode = m_zoomMode == ZoomMode::ActualSize ? ZoomMode::Fit : ZoomMode::ActualSize;
    UpdateScrollBars();

    // Start centered when entering actual size.
    const ViewLayout layout = ComputeLayout();
    m_panX = layout.maxPanX / 2.0f;
    m_panY = layout.maxPanY / 2.0f;
    UpdateView();
}

void MainWindow::OnLButtonDown(UINT /*flags*/, CPoint point)
{
    const ViewLayout layout = ComputeLayout();
    if (layout.maxPanX > 0.0f || layout.maxPanY > 0.0f)
    {
        m_dragging = true;
        m_dragLast = point;
        SetCapture();
    }
}

void MainWindow::OnMouseMove(UINT /*flags*/, CPoint point)
{
    if (!m_dragging)
    {
        return;
    }
    m_panX -= static_cast<float>(point.x - m_dragLast.x);
    m_panY -= static_cast<float>(point.y - m_dragLast.y);
    m_dragLast = point;
    UpdateView();
}

void MainWindow::OnLButtonUp(UINT /*flags*/, CPoint /*point*/)
{
    if (m_dragging)
    {
        m_dragging = false;
        ReleaseCapture();
    }
}

void MainWindow::OnCaptureChanged(HWND /*newCapture*/)
{
    m_dragging = false;
}

BOOL MainWindow::OnMouseWheel(UINT flags, short delta, CPoint screenPoint)
{
    CPoint point = screenPoint;
    ScreenToClient(&point);
    if (flags & MK_CONTROL)
    {
        StepZoom(delta > 0 ? +1 : -1, point);
    }
    else
    {
        m_panY -= static_cast<float>(delta) / WHEEL_DELTA * kWheelScroll * DpiScale();
        UpdateView();
    }
    return TRUE;
}

void MainWindow::OnScroll(int bar, int code)
{
    const ViewLayout layout = ComputeLayout();
    float& pan = bar == SB_HORZ ? m_panX : m_panY;
    const float maxPan = bar == SB_HORZ ? layout.maxPanX : layout.maxPanY;

    CRect rc;
    GetClientRect(&rc);
    const float page = static_cast<float>(bar == SB_HORZ ? rc.Width() : rc.Height());

    switch (code)
    {
    case SB_LINELEFT:  // == SB_LINEUP
        pan -= kScrollLine * DpiScale();
        break;
    case SB_LINERIGHT:  // == SB_LINEDOWN
        pan += kScrollLine * DpiScale();
        break;
    case SB_PAGELEFT:  // == SB_PAGEUP
        pan -= page;
        break;
    case SB_PAGERIGHT:  // == SB_PAGEDOWN
        pan += page;
        break;
    case SB_LEFT:  // == SB_TOP
        pan = 0.0f;
        break;
    case SB_RIGHT:  // == SB_BOTTOM
        pan = maxPan;
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
    {
        // 32-bit position; the 16-bit pos parameter would truncate.
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(bar, &si);
        pan = static_cast<float>(si.nTrackPos);
        break;
    }
    default:
        return;
    }
    UpdateView();
}

void MainWindow::OnHScroll(int code, short /*pos*/, HWND /*scrollBar*/)
{
    OnScroll(SB_HORZ, code);
}

void MainWindow::OnVScroll(int code, short /*pos*/, HWND /*scrollBar*/)
{
    OnScroll(SB_VERT, code);
}

void MainWindow::OnDropFiles(HDROP drop)
{
    auto cleanup = wil::scope_exit([&] { DragFinish(drop); });

    const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
    if (length == 0)
    {
        return;
    }
    std::wstring path(length + 1, L'\0');
    if (DragQueryFileW(drop, 0, path.data(), length + 1) == 0)
    {
        return;
    }
    path.resize(length);
    SetForegroundWindow(m_hWnd);
    LoadFile(std::filesystem::path(std::move(path)));
}

LRESULT MainWindow::OnImageLoaded(UINT /*msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/,
                                  BOOL& /*handled*/)
{
    auto result = m_loader->TakeResult();
    if (!result || result->generation != m_expectedGeneration)
    {
        return 0;  // stale result; a newer load is on its way
    }

    if (FAILED(result->hr))
    {
        // Keep the previous image (if any); the message overlays it.
        SetStatusError(result->hr);
    }
    else
    {
        m_cpuImage = std::move(result->image);
        m_bitmap.reset();  // recreated from m_cpuImage on next render
        m_state = ViewState::Loaded;
        m_panX = 0.0f;
        m_panY = 0.0f;
        UpdateScrollBars();  // zoom mode carries over; ranges follow the new image
        SetWindowTextW((result->path.filename().wstring() + L" - "
                        + LoadStringResource(IDS_APP_TITLE)).c_str());
    }
    Invalidate(FALSE);
    return 0;
}

LRESULT MainWindow::OnFolderScanned(UINT /*msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/,
                                    BOOL& /*handled*/)
{
    auto result = m_scanner->TakeResult();
    if (!result || result->generation != m_expectedScanGeneration)
    {
        return 0;  // stale scan; a newer one is on its way
    }
    m_folderFiles = std::move(result->files);
    m_currentIndex = FindFolderIndex(m_currentPath);
    return 0;
}

void MainWindow::SetStatusError(HRESULT hr)
{
    m_state = ViewState::Error;
    m_statusText = std::format(L"{} (0x{:08X})", LoadStringResource(ErrorStringId(hr)),
                               static_cast<unsigned>(hr));
}

void MainWindow::OnSize(UINT /*type*/, CSize size)
{
    if (m_renderTarget && size.cx > 0 && size.cy > 0)
    {
        m_renderTarget->Resize(D2D1::SizeU(size.cx, size.cy));
    }
    UpdateScrollBars();
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
    // Stop and join the workers before the window goes away.
    m_loader.reset();
    m_scanner.reset();
    DiscardDeviceResources();
    PostQuitMessage(0);
}

HRESULT MainWindow::CreateDeviceResources()
{
    if (!m_renderTarget)
    {
        CRect rc;
        GetClientRect(&rc);
        // Force 96 DPI so 1 DIP == 1 physical pixel; scaling is handled
        // explicitly through the view transform.
        RETURN_IF_FAILED(m_d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(),
                                         96.0f, 96.0f),
            D2D1::HwndRenderTargetProperties(m_hWnd,
                                             D2D1::SizeU(rc.Width(), rc.Height())),
            m_renderTarget.put()));
    }
    if (!m_textBrush)
    {
        RETURN_IF_FAILED(m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), m_textBrush.put()));
    }
    return S_OK;
}

void MainWindow::DiscardDeviceResources()
{
    m_bitmap.reset();
    m_textBrush.reset();
    m_renderTarget.reset();
}

HRESULT MainWindow::CreateBitmapFromCpuImage()
{
    const UINT32 maxSize = m_renderTarget->GetMaximumBitmapSize();
    RETURN_HR_IF(kHrImageTooLarge, m_cpuImage.width > maxSize || m_cpuImage.height > maxSize);

    return m_renderTarget->CreateBitmap(
        D2D1::SizeU(m_cpuImage.width, m_cpuImage.height), m_cpuImage.pixels.data(),
        m_cpuImage.stride,
        D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        m_bitmap.put());
}

void MainWindow::Render()
{
    if (FAILED(CreateDeviceResources()))
    {
        return;
    }

    if (!m_bitmap && m_cpuImage)
    {
        const HRESULT hr = CreateBitmapFromCpuImage();
        if (FAILED(hr))
        {
            m_cpuImage = {};  // don't retry on every frame
            SetStatusError(hr);
        }
    }

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));

    // The previous image stays up while the next one loads (or fails); the
    // status text overlays it, which avoids flicker when flipping quickly.
    if (m_bitmap)
    {
        const ViewLayout layout = ComputeLayout();
        m_renderTarget->DrawBitmap(
            m_bitmap.get(),
            D2D1::RectF(layout.destX, layout.destY, layout.destX + layout.displayWidth,
                        layout.destY + layout.displayHeight),
            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    if (m_state == ViewState::Loading || m_state == ViewState::Error)
    {
        DrawStatusText();
    }

    const HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        DiscardDeviceResources();
        Invalidate(FALSE);
    }
}

void MainWindow::DrawStatusText()
{
    if (m_statusText.empty())
    {
        return;
    }
    const D2D1_SIZE_F size = m_renderTarget->GetSize();

    // Semi-transparent backplate keeps the text readable over any image.
    const float halfBandHeight = 24.0f * DpiScale();
    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black, 0.6f));
    m_renderTarget->FillRectangle(
        D2D1::RectF(0.0f, size.height / 2.0f - halfBandHeight, size.width,
                    size.height / 2.0f + halfBandHeight),
        m_textBrush.get());

    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    m_renderTarget->DrawText(m_statusText.c_str(), static_cast<UINT32>(m_statusText.size()),
                             m_textFormat.get(), D2D1::RectF(0.0f, 0.0f, size.width, size.height),
                             m_textBrush.get());
}
