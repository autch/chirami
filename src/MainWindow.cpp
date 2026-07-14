#include "MainWindow.h"
#include "resource.h"

#include <shellapi.h>  // DragAcceptFiles, DragQueryFileW

#include <algorithm>
#include <format>

namespace
{

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
    THROW_IF_FAILED(m_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"", m_textFormat.put()));
    THROW_IF_FAILED(m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
    THROW_IF_FAILED(m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));

    m_loader = std::make_unique<ImageLoader>(m_hWnd, WM_APP_IMAGE_LOADED);
    m_scanner = std::make_unique<FolderScanner>(m_hWnd, WM_APP_FOLDER_SCANNED);

    DragAcceptFiles(TRUE);
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
    switch (key)
    {
    case VK_LEFT:
        NavigateBy(-1);
        break;
    case VK_RIGHT:
        NavigateBy(+1);
        break;
    default:
        break;
    }
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
        // explicitly (fit rect now, dot-by-dot display later).
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

D2D1_RECT_F MainWindow::ComputeFitRect(D2D1_SIZE_F imageSize, D2D1_SIZE_F clientSize)
{
    // Shrink to fit while keeping the aspect ratio, never upscale, and center
    // either way. Coordinates are DIPs, which equal physical pixels because
    // the render target is created at 96 DPI.
    const float scale = std::min({1.0f, clientSize.width / imageSize.width,
                                  clientSize.height / imageSize.height});
    const float width = imageSize.width * scale;
    const float height = imageSize.height * scale;
    const float left = (clientSize.width - width) / 2.0f;
    const float top = (clientSize.height - height) / 2.0f;
    return D2D1::RectF(left, top, left + width, top + height);
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
        m_renderTarget->DrawBitmap(
            m_bitmap.get(), ComputeFitRect(m_bitmap->GetSize(), m_renderTarget->GetSize()),
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
    constexpr float kHalfBandHeight = 24.0f;
    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black, 0.6f));
    m_renderTarget->FillRectangle(
        D2D1::RectF(0.0f, size.height / 2.0f - kHalfBandHeight, size.width,
                    size.height / 2.0f + kHalfBandHeight),
        m_textBrush.get());

    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    m_renderTarget->DrawText(m_statusText.c_str(), static_cast<UINT32>(m_statusText.size()),
                             m_textFormat.get(), D2D1::RectF(0.0f, 0.0f, size.width, size.height),
                             m_textBrush.get());
}
