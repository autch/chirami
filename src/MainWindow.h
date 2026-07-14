#pragma once

#include "framework.h"
#include "FolderScanner.h"
#include "ImageLoader.h"
#include "LoadedImage.h"
#include "resource.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

inline constexpr UINT WM_APP_IMAGE_LOADED = WM_APP + 1;
inline constexpr UINT WM_APP_FOLDER_SCANNED = WM_APP + 2;

class MainWindow : public CWindowImpl<MainWindow>
{
public:
    DECLARE_WND_CLASS_EX(L"ChiramiMainWindow", CS_HREDRAW | CS_VREDRAW, -1)

    BEGIN_MSG_MAP(MainWindow)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_SIZE(OnSize)
        MSG_WM_PAINT(OnPaint)
        MSG_WM_KEYDOWN(OnKeyDown)
        MSG_WM_LBUTTONDOWN(OnLButtonDown)
        MSG_WM_LBUTTONUP(OnLButtonUp)
        MSG_WM_MOUSEMOVE(OnMouseMove)
        MSG_WM_MOUSEWHEEL(OnMouseWheel)
        MSG_WM_CAPTURECHANGED(OnCaptureChanged)
        MSG_WM_HSCROLL(OnHScroll)
        MSG_WM_VSCROLL(OnVScroll)
        MSG_WM_DROPFILES(OnDropFiles)
        MSG_WM_INITMENUPOPUP(OnInitMenuPopup)
        MSG_WM_DESTROY(OnDestroy)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER(WM_APP_IMAGE_LOADED, OnImageLoaded)
        MESSAGE_HANDLER(WM_APP_FOLDER_SCANNED, OnFolderScanned)
        COMMAND_ID_HANDLER(IDM_FILE_OPEN, OnFileOpen)
        COMMAND_ID_HANDLER(IDM_FILE_EXIT, OnFileExit)
        COMMAND_ID_HANDLER(IDM_VIEW_FIT, OnViewFit)
        COMMAND_ID_HANDLER(IDM_VIEW_ACTUAL, OnViewActual)
        COMMAND_ID_HANDLER(IDM_VIEW_ZOOMIN, OnViewZoomIn)
        COMMAND_ID_HANDLER(IDM_VIEW_ZOOMOUT, OnViewZoomOut)
        COMMAND_ID_HANDLER(IDM_VIEW_FULLSCREEN, OnViewFullscreen)
        COMMAND_ID_HANDLER(IDM_HELP_ABOUT, OnHelpAbout)
    END_MSG_MAP()

    void LoadFile(std::filesystem::path path);

private:
    enum class ViewState
    {
        Empty,    // no file requested
        Loading,  // waiting for the loader (previous image, if any, stays up)
        Loaded,   // m_cpuImage valid, drawn from m_bitmap
        Error     // m_statusText overlays the previous image, if any
    };

    enum class ZoomMode
    {
        Fit,         // shrink to fit the window, never upscale (scrollbars off)
        ActualSize,  // dot-by-dot
        Custom       // user-driven zoom factor (m_zoomScale)
    };

    // Where and how large the image appears in the client area. Pan values
    // are clamped against maxPan when this is computed.
    struct ViewLayout
    {
        float scale = 0.0f;
        float displayWidth = 0.0f;   // image size * scale
        float displayHeight = 0.0f;
        float destX = 0.0f;          // top-left of the image in client coords
        float destY = 0.0f;
        float maxPanX = 0.0f;        // 0 when the image fits on that axis
        float maxPanY = 0.0f;
    };

    int OnCreate(LPCREATESTRUCT createStruct);
    void OnSize(UINT type, CSize size);
    void OnPaint(CDCHandle dc);
    void OnKeyDown(UINT key, UINT repeatCount, UINT flags);
    void OnLButtonDown(UINT flags, CPoint point);
    void OnLButtonUp(UINT flags, CPoint point);
    void OnMouseMove(UINT flags, CPoint point);
    BOOL OnMouseWheel(UINT flags, short delta, CPoint screenPoint);
    void OnCaptureChanged(HWND newCapture);
    void OnHScroll(int code, short pos, HWND scrollBar);
    void OnVScroll(int code, short pos, HWND scrollBar);
    void OnDropFiles(HDROP drop);
    void OnInitMenuPopup(CMenuHandle menu, UINT index, BOOL sysMenu);
    void OnDestroy();
    LRESULT OnFileOpen(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnFileExit(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnViewFit(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnViewActual(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnViewZoomIn(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnViewZoomOut(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnViewFullscreen(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnHelpAbout(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnDpiChanged(UINT msg, WPARAM wParam, LPARAM lParam, BOOL& handled);
    LRESULT OnImageLoaded(UINT msg, WPARAM wParam, LPARAM lParam, BOOL& handled);
    LRESULT OnFolderScanned(UINT msg, WPARAM wParam, LPARAM lParam, BOOL& handled);

    void NavigateBy(int delta);
    ptrdiff_t FindFolderIndex(const std::filesystem::path& path) const;
    void ToggleFullscreen();
    void ApplyAutoZoomForNewImage();

    ViewLayout ComputeLayout();
    void UpdateScrollBars();
    void SyncScrollPositions();
    void UpdateView();
    void ApplyZoom(float newScale, CPoint anchor);
    void StepZoom(int direction, CPoint anchor);
    void SetZoomMode(ZoomMode mode);
    void OnScroll(int bar, int code);

    float DpiScale() const { return static_cast<float>(m_dpi) / 96.0f; }
    HRESULT CreateTextFormat();

    HRESULT CreateDeviceResources();
    void DiscardDeviceResources();
    HRESULT CreateBitmapFromCpuImage();
    void Render();
    void DrawStatusText();
    void SetStatusError(HRESULT hr);

    // Device-independent resources (created once in OnCreate)
    wil::com_ptr<ID2D1Factory> m_d2dFactory;
    wil::com_ptr<IWICImagingFactory> m_wicFactory;
    wil::com_ptr<IDWriteFactory> m_dwriteFactory;
    wil::com_ptr<IDWriteTextFormat> m_textFormat;

    // Device-dependent resources (recreated after D2DERR_RECREATE_TARGET)
    wil::com_ptr<ID2D1HwndRenderTarget> m_renderTarget;
    wil::com_ptr<ID2D1SolidColorBrush> m_textBrush;
    wil::com_ptr<ID2D1Bitmap> m_bitmap;

    std::unique_ptr<ImageLoader> m_loader;
    std::unique_ptr<FolderScanner> m_scanner;
    LoadedImage m_cpuImage;  // kept after upload so device loss needs no re-decode
    ViewState m_state = ViewState::Empty;
    std::wstring m_statusText;
    uint64_t m_expectedGeneration = 0;

    // Folder navigation (Phase 1 step 3)
    std::filesystem::path m_currentPath;
    std::filesystem::path m_currentFolder;
    std::vector<std::filesystem::path> m_folderFiles;  // sorted, from FolderScanner
    ptrdiff_t m_currentIndex = -1;                     // -1: unknown / not in list
    uint64_t m_expectedScanGeneration = 0;
    bool m_edgeWarned = false;  // beeped at the list edge; next attempt wraps

    // View transform (Phase 1 steps 5/6). Pan is the top-left of the
    // viewport within the scaled image, in pixels (1 DIP == 1 pixel).
    ZoomMode m_zoomMode = ZoomMode::Fit;
    float m_zoomScale = 1.0f;  // meaningful in Custom mode only
    float m_panX = 0.0f;
    float m_panY = 0.0f;
    bool m_dragging = false;
    CPoint m_dragLast{};
    bool m_updatingScrollBars = false;

    // Per-monitor DPI. The image itself always maps 1 image pixel to 1
    // physical pixel at 100%; DPI only scales our own UI (text, scroll
    // steps), never the image.
    UINT m_dpi = USER_DEFAULT_SCREEN_DPI;

    // Fullscreen (Phase 1 step 7)
    bool m_fullscreen = false;
    WINDOWPLACEMENT m_restorePlacement{};  // window state before fullscreen

    // Menu bar (Phase 2 step 8). Kept here while detached in fullscreen.
    CMenuHandle m_menu;

    void ShowFileOpenDialog();
    void ShowAboutBox();
    void StepZoomAtCenter(int direction);
};
