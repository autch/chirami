#pragma once

#include "framework.h"
#include "FolderScanner.h"
#include "ImageCache.h"
#include "ImageLoader.h"
#include "ImageSaver.h"
#include "LoadedImage.h"
#include "SelectionState.h"
#include "Settings.h"
#include "resource.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

inline constexpr UINT WM_APP_IMAGE_LOADED = WM_APP + 1;
inline constexpr UINT WM_APP_FOLDER_SCANNED = WM_APP + 2;
inline constexpr UINT WM_APP_SAVE_DONE = WM_APP + 3;
inline constexpr UINT WM_APP_PREFETCH_DONE = WM_APP + 4;

class MainWindow : public CWindowImpl<MainWindow>
{
public:
    DECLARE_WND_CLASS_EX(L"ChiramiMainWindow", CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, -1)

    BEGIN_MSG_MAP(MainWindow)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_SIZE(OnSize)
        MSG_WM_PAINT(OnPaint)
        MSG_WM_KEYDOWN(OnKeyDown)
        MSG_WM_LBUTTONDOWN(OnLButtonDown)
        MSG_WM_LBUTTONDBLCLK(OnLButtonDblClk)
        MSG_WM_LBUTTONUP(OnLButtonUp)
        MSG_WM_MOUSEMOVE(OnMouseMove)
        MSG_WM_MOUSEWHEEL(OnMouseWheel)
        MSG_WM_SETCURSOR(OnSetCursor)
        MSG_WM_CAPTURECHANGED(OnCaptureChanged)
        MSG_WM_HSCROLL(OnHScroll)
        MSG_WM_VSCROLL(OnVScroll)
        MSG_WM_DROPFILES(OnDropFiles)
        MSG_WM_INITMENUPOPUP(OnInitMenuPopup)
        MSG_WM_DESTROY(OnDestroy)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER(WM_APP_IMAGE_LOADED, OnImageLoaded)
        MESSAGE_HANDLER(WM_APP_FOLDER_SCANNED, OnFolderScanned)
        MESSAGE_HANDLER(WM_APP_SAVE_DONE, OnSaveDone)
        MESSAGE_HANDLER(WM_APP_PREFETCH_DONE, OnPrefetchDone)
        COMMAND_ID_HANDLER(IDM_FILE_OPEN, OnFileOpen)
        COMMAND_ID_HANDLER(IDM_FILE_SAVEAS, OnFileSaveAs)
        COMMAND_ID_HANDLER(IDM_FILE_EXIT, OnFileExit)
        COMMAND_ID_HANDLER(IDM_EDIT_PASTE, OnEditPaste)
        COMMAND_ID_HANDLER(IDM_EDIT_CROP, OnEditCrop)
        COMMAND_ID_HANDLER(IDM_EDIT_BLACKOUT, OnEditBlackout)
        COMMAND_ID_HANDLER(IDM_EDIT_RESIZE, OnEditResize)
        COMMAND_RANGE_HANDLER(IDM_EDIT_ROTATE_CW, IDM_EDIT_FLIP_V, OnEditTransform)
        COMMAND_RANGE_HANDLER(IDM_SORT_NAME, IDM_SORT_DESC, OnSortChanged)
        COMMAND_ID_HANDLER(IDM_VIEW_FIT, OnViewFit)
        COMMAND_ID_HANDLER(IDM_VIEW_ACTUAL, OnViewActual)
        COMMAND_ID_HANDLER(IDM_VIEW_ZOOMIN, OnViewZoomIn)
        COMMAND_ID_HANDLER(IDM_VIEW_ZOOMOUT, OnViewZoomOut)
        COMMAND_ID_HANDLER(IDM_VIEW_FULLSCREEN, OnViewFullscreen)
        COMMAND_ID_HANDLER(IDM_HELP_ABOUT, OnHelpAbout)
    END_MSG_MAP()

    void LoadFile(std::filesystem::path path);

    // Call before Create(); the language override is applied in wWinMain.
    void InitSettings(Settings settings) { m_settings = std::move(settings); }

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
    void OnLButtonDblClk(UINT flags, CPoint point);
    void OnLButtonUp(UINT flags, CPoint point);
    void OnMouseMove(UINT flags, CPoint point);
    BOOL OnMouseWheel(UINT flags, short delta, CPoint screenPoint);
    BOOL OnSetCursor(CWindow window, UINT hitTest, UINT message);
    void OnCaptureChanged(HWND newCapture);
    void OnHScroll(int code, short pos, HWND scrollBar);
    void OnVScroll(int code, short pos, HWND scrollBar);
    void OnDropFiles(HDROP drop);
    void OnInitMenuPopup(CMenuHandle menu, UINT index, BOOL sysMenu);
    void OnDestroy();
    LRESULT OnFileOpen(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnFileSaveAs(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnFileExit(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnEditPaste(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnEditCrop(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnEditBlackout(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnEditResize(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnEditTransform(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnSortChanged(WORD code, WORD id, HWND control, BOOL& handled);
    LRESULT OnSaveDone(UINT msg, WPARAM wParam, LPARAM lParam, BOOL& handled);
    LRESULT OnPrefetchDone(UINT msg, WPARAM wParam, LPARAM lParam, BOOL& handled);
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
    void OpenFolder(std::filesystem::path folder);
    void ToggleFullscreen();
    void ApplyAutoZoomForNewImage();
    void AutoFitWindowAfterZoom();
    void ResizeWindowToClient(int clientWidth, int clientHeight);

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
    bool m_openFirstAfterScan = false;  // folder was opened; show its first image

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

    std::unique_ptr<ImageSaver> m_saver;
    Settings m_settings;

    // Prefetching (Phase 3): a second loader fills m_cache with the current
    // file's neighbors so arrow-key navigation displays instantly.
    std::unique_ptr<ImageLoader> m_prefetcher;
    ImageCache m_cache;
    std::filesystem::path m_displayedPath;   // identity of m_cpuImage; empty
                                             // for clipboard or edited images
    std::filesystem::path m_prefetchPath;    // request in flight, if any
    uint64_t m_prefetchGeneration = 0;
    std::vector<std::filesystem::path> m_prefetchFailed;  // don't retry these

    void DisplayImage(const std::wstring& displayName, LoadedImage image);
    void TriggerPrefetch();

    // Rubber-band selection for crop / blackout (Phase 3 step 16)
    enum class SelectionPurpose
    {
        None,
        Crop,
        Blackout,
    };
    SelectionPurpose m_selectionPurpose = SelectionPurpose::None;
    SelectionState m_selection;
    wil::com_ptr<ID2D1StrokeStyle> m_dashStroke;  // device-independent

    // Moving the selection only starts after the pointer travels past the
    // system drag threshold, so clicks (and double-clicks) never nudge it.
    bool m_movePending = false;
    CPoint m_movePendingPoint{};

    bool InSelectionMode() const { return m_selectionPurpose != SelectionPurpose::None; }
    void EnterSelectionMode(SelectionPurpose purpose);
    void ExitSelectionMode();
    void ApplySelection();
    void DrawSelectionOverlay(const ViewLayout& layout);
    D2D1_POINT_2F ClientToImage(CPoint point, const ViewLayout& layout) const;
    float SelectionHitTolerance(const ViewLayout& layout) const;

    void ShowFileOpenDialog();
    void ShowFileSaveDialog();
    void ShowResizeDialog();
    void ShowAboutBox();
    void StepZoomAtCenter(int direction);
    void PasteFromClipboard();
    std::vector<uint8_t> ReadClipboardImageBlob();
    void ApplyTransform(WORD commandId);
    FolderScanner::SortSpec CurrentSortSpec() const;
    void RescanFolder();
};
