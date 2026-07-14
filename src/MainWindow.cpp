#include "MainWindow.h"
#include "ImageTransform.h"
#include "ResizeDialog.h"
#include "resource.h"

#include <shellapi.h>   // DragAcceptFiles, DragQueryFileW
#include <shobjidl.h>   // IFileOpenDialog, IFileSaveDialog

#include <algorithm>
#include <cmath>
#include <cwctype>
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

GUID ContainerFormatFromExtension(std::wstring extension)
{
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (extension == L".png")
    {
        return GUID_ContainerFormatPng;
    }
    if (extension == L".jpg" || extension == L".jpeg" || extension == L".jfif")
    {
        return GUID_ContainerFormatJpeg;
    }
    if (extension == L".bmp")
    {
        return GUID_ContainerFormatBmp;
    }
    if (extension == L".tif" || extension == L".tiff")
    {
        return GUID_ContainerFormatTiff;
    }
    return GUID_NULL;
}

bool PathsEqualNoCase(const std::filesystem::path& a, const std::filesystem::path& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

}  // namespace

int MainWindow::OnCreate(LPCREATESTRUCT /*createStruct*/)
{
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put()));
    m_wicFactory = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);
    THROW_IF_FAILED(m_d2dFactory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                    D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f,
                                    D2D1_DASH_STYLE_DASH, 0.0f),
        nullptr, 0, m_dashStroke.put()));

    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(m_dwriteFactory.put())));
    m_dpi = GetDpiForWindow(m_hWnd);
    THROW_IF_FAILED(CreateTextFormat());

    m_loader = std::make_unique<ImageLoader>(m_hWnd, WM_APP_IMAGE_LOADED);
    m_scanner = std::make_unique<FolderScanner>(m_hWnd, WM_APP_FOLDER_SCANNED);
    m_saver = std::make_unique<ImageSaver>(m_hWnd, WM_APP_SAVE_DONE);
    m_prefetcher = std::make_unique<ImageLoader>(m_hWnd, WM_APP_PREFETCH_DONE);

    // Picks the resource language set up by SetThreadUILanguage.
    m_menu = CMenuHandle(LoadMenuW(_Module.GetResourceInstance(), MAKEINTRESOURCEW(IDR_MAINMENU)));
    SetMenu(m_menu);

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

    // A dropped or association-launched folder shows its first image. The
    // attribute query is the one synchronous touch of the path on the UI
    // thread; everything after goes through the workers.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        OpenFolder(std::move(path));
        return;
    }

    m_currentPath = std::move(path);
    m_edgeWarned = false;
    m_openFirstAfterScan = false;  // an explicit file wins over a pending folder open

    if (auto folder = m_currentPath.parent_path(); folder != m_currentFolder)
    {
        m_currentFolder = std::move(folder);
        m_folderFiles.clear();
        m_currentIndex = -1;
        m_prefetchFailed.clear();
        m_expectedScanGeneration = m_scanner->RequestScan(m_currentFolder, CurrentSortSpec());
    }
    else if (!m_folderFiles.empty())
    {
        m_currentIndex = FindFolderIndex(m_currentPath);
    }

    if (auto cached = m_cache.Take(m_currentPath))
    {
        // Prefetched: display immediately, no loader round trip. Generation 0
        // never matches a real one, so any in-flight load result is dropped.
        m_expectedGeneration = 0;
        DisplayImage(m_currentPath.filename().wstring(), std::move(*cached));
        return;
    }
    m_expectedGeneration = m_loader->RequestLoad(m_currentPath);
    m_state = ViewState::Loading;
    m_statusText = LoadStringResource(IDS_STATUS_LOADING);
    Invalidate(FALSE);
}

void MainWindow::OpenFolder(std::filesystem::path folder)
{
    m_currentPath.clear();
    m_currentFolder = std::move(folder);
    m_folderFiles.clear();
    m_currentIndex = -1;
    m_prefetchFailed.clear();
    m_edgeWarned = false;
    m_openFirstAfterScan = true;
    m_expectedScanGeneration = m_scanner->RequestScan(m_currentFolder, CurrentSortSpec());

    // The scan may take a moment on network folders; say so.
    m_state = ViewState::Loading;
    m_statusText = LoadStringResource(IDS_STATUS_LOADING);
    Invalidate(FALSE);
}

void MainWindow::DisplayImage(const std::wstring& displayName, LoadedImage image)
{
    ExitSelectionMode();  // a selection never survives an image change

    // Recycle the outgoing image so navigating back is instant. Clipboard
    // content and edited (rotated/flipped) images have no on-disk identity
    // and are not cached.
    if (m_cpuImage && !m_displayedPath.empty())
    {
        m_cache.Put(std::move(m_displayedPath), std::move(m_cpuImage));
    }
    m_displayedPath = m_currentPath;

    m_cpuImage = std::move(image);
    m_tiles.clear();  // recreated from m_cpuImage on next render
    m_state = ViewState::Loaded;
    m_panX = 0.0f;
    m_panY = 0.0f;
    ApplyAutoZoomForNewImage();
    UpdateScrollBars();
    SetWindowTextW((displayName + L" - " + LoadStringResource(IDS_APP_TITLE)).c_str());
    Invalidate(FALSE);

    TriggerPrefetch();
}

void MainWindow::TriggerPrefetch()
{
    if (m_folderFiles.empty() || m_currentIndex < 0)
    {
        return;
    }

    // Next first (the more likely direction), then previous.
    std::vector<std::filesystem::path> wanted;
    if (m_currentIndex + 1 < static_cast<ptrdiff_t>(m_folderFiles.size()))
    {
        wanted.push_back(m_folderFiles[static_cast<size_t>(m_currentIndex) + 1]);
    }
    if (m_currentIndex > 0)
    {
        wanted.push_back(m_folderFiles[static_cast<size_t>(m_currentIndex) - 1]);
    }
    m_cache.KeepOnly(wanted);

    for (const auto& path : wanted)
    {
        if (m_cache.Contains(path) || PathsEqualNoCase(path, m_displayedPath))
        {
            continue;
        }
        if (std::ranges::any_of(m_prefetchFailed,
                                [&](const auto& failed) { return PathsEqualNoCase(failed, path); }))
        {
            continue;
        }
        if (PathsEqualNoCase(path, m_prefetchPath))
        {
            return;  // already being fetched
        }
        m_prefetchPath = path;
        m_prefetchGeneration = m_prefetcher->RequestLoad(path);
        return;  // one at a time; completion re-triggers for the other one
    }
}

LRESULT MainWindow::OnPrefetchDone(UINT /*msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/,
                                   BOOL& /*handled*/)
{
    auto result = m_prefetcher->TakeResult();
    if (!result || result->generation != m_prefetchGeneration)
    {
        return 0;
    }
    m_prefetchPath.clear();
    if (SUCCEEDED(result->hr))
    {
        m_cache.Put(result->path, std::move(result->image));
    }
    else
    {
        m_prefetchFailed.push_back(result->path);  // no retry loops
    }
    TriggerPrefetch();
    return 0;
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
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (InSelectionMode())
    {
        switch (key)
        {
        case VK_ESCAPE:
            ExitSelectionMode();
            return;
        case VK_RETURN:
            ApplySelection();
            return;
        // Navigation and pixel edits would invalidate the selection; zoom
        // and pan keys remain available.
        case VK_LEFT:
        case VK_RIGHT:
            return;
        case 'R':
        case 'L':
        case 'H':
            if (!control)
            {
                return;
            }
            break;
        case 'V':
            if (!control)
            {
                return;
            }
            break;
        default:
            break;
        }
    }

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
        StepZoomAtCenter(+1);
        break;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        StepZoomAtCenter(-1);
        break;
    case '1':
    case VK_NUMPAD1:
        SetZoomMode(ZoomMode::ActualSize);
        break;
    case '0':
    case VK_NUMPAD0:
        SetZoomMode(ZoomMode::Fit);
        break;
    case 'O':
        if (control)
        {
            ShowFileOpenDialog();
        }
        break;
    case 'S':
        if (control)
        {
            ShowFileSaveDialog();
        }
        break;
    case 'R':
        if (control)
        {
            ShowResizeDialog();
        }
        else
        {
            ApplyTransform(IDM_EDIT_ROTATE_CW);
        }
        break;
    case 'L':
        if (!control)
        {
            ApplyTransform(IDM_EDIT_ROTATE_CCW);
        }
        break;
    case 'H':
        if (!control)
        {
            ApplyTransform(IDM_EDIT_FLIP_H);
        }
        break;
    case 'V':
        if (control)
        {
            PasteFromClipboard();
        }
        else
        {
            ApplyTransform(IDM_EDIT_FLIP_V);
        }
        break;
    case 'C':
        if (!control)
        {
            EnterSelectionMode(SelectionPurpose::Crop);
        }
        break;
    case 'B':
        if (!control)
        {
            EnterSelectionMode(SelectionPurpose::Blackout);
        }
        break;
    case VK_F11:
        ToggleFullscreen();
        break;
    case VK_ESCAPE:
        if (m_fullscreen)
        {
            ToggleFullscreen();
        }
        break;
    default:
        break;
    }
}

void MainWindow::StepZoomAtCenter(int direction)
{
    CRect rc;
    GetClientRect(&rc);
    StepZoom(direction, CPoint(rc.Width() / 2, rc.Height() / 2));
}

void MainWindow::OnInitMenuPopup(CMenuHandle menu, UINT /*index*/, BOOL sysMenu)
{
    if (sysMenu || menu.IsNull())
    {
        return;
    }
    // All calls below are no-ops for popups that don't contain the ids.
    const bool fit = m_zoomMode == ZoomMode::Fit;
    const bool actual = m_zoomMode == ZoomMode::ActualSize;
    menu.CheckMenuItem(IDM_VIEW_FIT, MF_BYCOMMAND | (fit ? MF_CHECKED : MF_UNCHECKED));
    menu.CheckMenuItem(IDM_VIEW_ACTUAL, MF_BYCOMMAND | (actual ? MF_CHECKED : MF_UNCHECKED));
    menu.CheckMenuItem(IDM_VIEW_FULLSCREEN,
                       MF_BYCOMMAND | (m_fullscreen ? MF_CHECKED : MF_UNCHECKED));

    const UINT imageState = MF_BYCOMMAND | (m_cpuImage ? MF_ENABLED : MF_GRAYED);
    menu.EnableMenuItem(IDM_FILE_SAVEAS, imageState);
    for (UINT id = IDM_EDIT_ROTATE_CW; id <= IDM_EDIT_RESIZE; ++id)
    {
        menu.EnableMenuItem(id, imageState);
    }
    menu.CheckMenuItem(IDM_EDIT_CROP,
                       MF_BYCOMMAND
                           | (m_selectionPurpose == SelectionPurpose::Crop ? MF_CHECKED
                                                                           : MF_UNCHECKED));
    menu.CheckMenuItem(IDM_EDIT_BLACKOUT,
                       MF_BYCOMMAND
                           | (m_selectionPurpose == SelectionPurpose::Blackout ? MF_CHECKED
                                                                               : MF_UNCHECKED));
    const bool canPaste = IsClipboardFormatAvailable(CF_DIB)
                          || IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG"));
    menu.EnableMenuItem(IDM_EDIT_PASTE, MF_BYCOMMAND | (canPaste ? MF_ENABLED : MF_GRAYED));

    menu.CheckMenuRadioItem(IDM_SORT_NAME, IDM_SORT_SIZE,
                            IDM_SORT_NAME + static_cast<UINT>(m_settings.sortKey), MF_BYCOMMAND);
    menu.CheckMenuRadioItem(IDM_SORT_ASC, IDM_SORT_DESC,
                            m_settings.sortDescending ? IDM_SORT_DESC : IDM_SORT_ASC,
                            MF_BYCOMMAND);
}

FolderScanner::SortSpec MainWindow::CurrentSortSpec() const
{
    return {m_settings.sortKey, m_settings.sortDescending};
}

void MainWindow::RescanFolder()
{
    if (!m_currentFolder.empty())
    {
        m_expectedScanGeneration = m_scanner->RequestScan(m_currentFolder, CurrentSortSpec());
    }
}

void MainWindow::EnterSelectionMode(SelectionPurpose purpose)
{
    if (!m_cpuImage)
    {
        return;
    }
    if (m_selectionPurpose == purpose)
    {
        ExitSelectionMode();  // the same command toggles the mode off
        return;
    }
    // Switching between crop and blackout keeps the current rectangle.
    m_selectionPurpose = purpose;
    Invalidate(FALSE);
}

void MainWindow::ExitSelectionMode()
{
    if (!InSelectionMode())
    {
        return;
    }
    if (m_selection.Dragging())
    {
        ReleaseCapture();
    }
    m_selection.Reset();
    m_selectionPurpose = SelectionPurpose::None;
    Invalidate(FALSE);
}

void MainWindow::ApplySelection()
{
    if (!m_cpuImage || !m_selection.HasRect())
    {
        MessageBeep(MB_OK);
        return;
    }
    const D2D1_RECT_F rect = m_selection.Rect();
    const auto left = static_cast<uint32_t>(std::clamp(
        std::lround(rect.left), 0L, static_cast<long>(m_cpuImage.width)));
    const auto top = static_cast<uint32_t>(std::clamp(
        std::lround(rect.top), 0L, static_cast<long>(m_cpuImage.height)));
    const auto right = static_cast<uint32_t>(std::clamp(
        std::lround(rect.right), 0L, static_cast<long>(m_cpuImage.width)));
    const auto bottom = static_cast<uint32_t>(std::clamp(
        std::lround(rect.bottom), 0L, static_cast<long>(m_cpuImage.height)));
    if (right <= left || bottom <= top)
    {
        MessageBeep(MB_OK);
        return;
    }

    // Baked into the pixels like rotate/flip: transient until saved, so the
    // on-disk identity is dropped and the result stays out of the cache.
    m_displayedPath.clear();
    m_tiles.clear();

    if (m_selectionPurpose == SelectionPurpose::Crop)
    {
        m_cpuImage = CropImage(m_cpuImage, left, top, right - left, bottom - top);
        ExitSelectionMode();
        ApplyAutoZoomForNewImage();
        UpdateScrollBars();
    }
    else
    {
        // Stay in the mode so several areas can be redacted in a row.
        FillRectBlack(m_cpuImage, left, top, right - left, bottom - top);
        m_selection.Reset();
    }
    Invalidate(FALSE);
}

D2D1_POINT_2F MainWindow::ClientToImage(CPoint point, const ViewLayout& layout) const
{
    return {(static_cast<float>(point.x) - layout.destX) / layout.scale,
            (static_cast<float>(point.y) - layout.destY) / layout.scale};
}

float MainWindow::SelectionHitTolerance(const ViewLayout& layout) const
{
    return 8.0f * DpiScale() / layout.scale;  // screen pixels -> image pixels
}

void MainWindow::ApplyTransform(WORD commandId)
{
    if (!m_cpuImage)
    {
        return;
    }
    ExitSelectionMode();  // the rectangle would no longer match the pixels
    switch (commandId)
    {
    case IDM_EDIT_ROTATE_CW:
        m_cpuImage = RotateImage90(m_cpuImage, true);
        break;
    case IDM_EDIT_ROTATE_CCW:
        m_cpuImage = RotateImage90(m_cpuImage, false);
        break;
    case IDM_EDIT_FLIP_H:
        FlipImageHorizontal(m_cpuImage);
        break;
    case IDM_EDIT_FLIP_V:
        FlipImageVertical(m_cpuImage);
        break;
    default:
        return;
    }
    // Edits are transient until saved; drop the on-disk identity so the
    // edited pixels never enter the prefetch cache.
    m_displayedPath.clear();
    m_tiles.clear();
    ApplyAutoZoomForNewImage();  // dimensions may have swapped
    UpdateScrollBars();
    Invalidate(FALSE);
}

std::vector<uint8_t> MainWindow::ReadClipboardImageBlob()
{
    std::vector<uint8_t> blob;
    if (!OpenClipboard())
    {
        return blob;
    }
    auto close = wil::scope_exit([] { CloseClipboard(); });

    // Prefer the "PNG" format (browsers and editors put it there; it keeps
    // alpha reliably), then fall back to CF_DIB wrapped as a .bmp blob so
    // both go through the same WIC decode path.
    if (HANDLE handle = GetClipboardData(RegisterClipboardFormatW(L"PNG")))
    {
        if (const void* data = GlobalLock(handle))
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            blob.assign(bytes, bytes + GlobalSize(handle));
            GlobalUnlock(handle);
            return blob;
        }
    }
    if (HANDLE handle = GetClipboardData(CF_DIB))
    {
        const size_t dibSize = GlobalSize(handle);
        if (const void* data = GlobalLock(handle); data && dibSize >= sizeof(BITMAPINFOHEADER))
        {
            const auto* header = static_cast<const BITMAPINFOHEADER*>(data);
            uint32_t offset = sizeof(BITMAPFILEHEADER) + header->biSize;
            if (header->biCompression == BI_BITFIELDS
                && header->biSize == sizeof(BITMAPINFOHEADER))
            {
                offset += 3 * sizeof(DWORD);  // color masks follow the header
            }
            uint32_t colors = header->biClrUsed;
            if (colors == 0 && header->biBitCount <= 8)
            {
                colors = 1u << header->biBitCount;
            }
            offset += colors * sizeof(RGBQUAD);

            BITMAPFILEHEADER fileHeader{};
            fileHeader.bfType = 0x4D42;  // "BM"
            fileHeader.bfSize = static_cast<DWORD>(sizeof(fileHeader) + dibSize);
            fileHeader.bfOffBits = offset;

            blob.resize(sizeof(fileHeader) + dibSize);
            std::memcpy(blob.data(), &fileHeader, sizeof(fileHeader));
            std::memcpy(blob.data() + sizeof(fileHeader), data, dibSize);
            GlobalUnlock(handle);
        }
    }
    return blob;
}

void MainWindow::PasteFromClipboard()
{
    std::vector<uint8_t> blob = ReadClipboardImageBlob();
    if (blob.empty())
    {
        MessageBeep(MB_OK);
        return;
    }
    // The pasted image has no file identity; arrow keys resume from the
    // first/last file of the previously scanned folder.
    m_currentPath.clear();
    m_currentIndex = -1;
    m_expectedGeneration =
        m_loader->RequestLoadFromMemory(std::move(blob), LoadStringResource(IDS_CLIPBOARD_NAME));
    m_state = ViewState::Loading;
    m_statusText = LoadStringResource(IDS_STATUS_LOADING);
    m_edgeWarned = false;
    Invalidate(FALSE);
}

void MainWindow::ShowFileSaveDialog()
try
{
    if (!m_cpuImage)
    {
        return;
    }
    auto dialog = wil::CoCreateInstance<IFileSaveDialog>(CLSID_FileSaveDialog);

    const std::wstring pngName = LoadStringResource(IDS_FILTER_PNG);
    const std::wstring jpegName = LoadStringResource(IDS_FILTER_JPEG);
    const std::wstring bmpName = LoadStringResource(IDS_FILTER_BMP);
    const std::wstring tiffName = LoadStringResource(IDS_FILTER_TIFF);
    const COMDLG_FILTERSPEC filters[] = {
        {pngName.c_str(), L"*.png"},
        {jpegName.c_str(), L"*.jpg;*.jpeg"},
        {bmpName.c_str(), L"*.bmp"},
        {tiffName.c_str(), L"*.tif;*.tiff"},
    };
    THROW_IF_FAILED(dialog->SetFileTypes(ARRAYSIZE(filters), filters));
    THROW_IF_FAILED(dialog->SetDefaultExtension(L"png"));
    if (!m_currentPath.empty())
    {
        THROW_IF_FAILED(dialog->SetFileName(m_currentPath.stem().c_str()));
    }

    const HRESULT hr = dialog->Show(m_hWnd);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        return;
    }
    THROW_IF_FAILED(hr);

    wil::com_ptr<IShellItem> item;
    THROW_IF_FAILED(dialog->GetResult(item.put()));
    wil::unique_cotaskmem_string pathString;
    THROW_IF_FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, pathString.put()));
    std::filesystem::path path(pathString.get());

    // The typed extension decides the format; fall back to the selected
    // filter for unknown extensions.
    GUID container = ContainerFormatFromExtension(path.extension().wstring());
    if (container == GUID_NULL)
    {
        static const GUID kByFilterIndex[] = {
            GUID_ContainerFormatPng,
            GUID_ContainerFormatJpeg,
            GUID_ContainerFormatBmp,
            GUID_ContainerFormatTiff,
        };
        UINT typeIndex = 1;  // 1-based
        (void)dialog->GetFileTypeIndex(&typeIndex);
        container = kByFilterIndex[std::clamp<UINT>(typeIndex, 1, ARRAYSIZE(kByFilterIndex)) - 1];
    }

    if (!m_saver->RequestSave(std::move(path), container, m_cpuImage))
    {
        MessageBeep(MB_OK);  // previous save still in progress
    }
}
CATCH_LOG()

LRESULT MainWindow::OnSaveDone(UINT /*msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/,
                               BOOL& /*handled*/)
{
    auto result = m_saver->TakeResult();
    if (result && FAILED(result->hr))
    {
        const std::wstring text =
            std::format(L"{} (0x{:08X})\n{}", LoadStringResource(IDS_ERR_SAVE),
                        static_cast<unsigned>(result->hr), result->path.wstring());
        MessageBoxW(text.c_str(), LoadStringResource(IDS_APP_TITLE).c_str(),
                    MB_OK | MB_ICONERROR);
    }
    return 0;
}

void MainWindow::ShowFileOpenDialog()
try
{
    auto dialog = wil::CoCreateInstance<IFileOpenDialog>(CLSID_FileOpenDialog);

    const std::wstring imagesName = LoadStringResource(IDS_FILTER_IMAGES);
    const std::wstring allName = LoadStringResource(IDS_FILTER_ALL);
    const COMDLG_FILTERSPEC filters[] = {
        // Browsing filter only; anything WIC can decode still loads (e.g.
        // via "All files"), so this static list doesn't need to be complete.
        {imagesName.c_str(),
         L"*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp;*.avif"},
        {allName.c_str(), L"*.*"},
    };
    THROW_IF_FAILED(dialog->SetFileTypes(ARRAYSIZE(filters), filters));

    const HRESULT hr = dialog->Show(m_hWnd);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        return;
    }
    THROW_IF_FAILED(hr);

    wil::com_ptr<IShellItem> item;
    THROW_IF_FAILED(dialog->GetResult(item.put()));
    wil::unique_cotaskmem_string path;
    THROW_IF_FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, path.put()));
    LoadFile(path.get());
}
CATCH_LOG()

void MainWindow::ShowResizeDialog()
{
    if (!m_cpuImage)
    {
        return;
    }
    ExitSelectionMode();  // a selection would no longer match the pixels

    ResizeDialog dialog(m_cpuImage.width, m_cpuImage.height);
    if (dialog.DoModal(m_hWnd) != IDOK)
    {
        return;
    }
    const uint32_t width = dialog.ResultWidth();
    const uint32_t height = dialog.ResultHeight();
    if (width == m_cpuImage.width && height == m_cpuImage.height)
    {
        return;
    }

    // Fant-filtered rescale on the UI thread; even 8K completes fast enough
    // for an explicit dialog-confirmed action.
    LoadedImage resized;
    const HRESULT hr = ResizeImage(m_wicFactory.get(), m_cpuImage, width, height, resized);
    if (FAILED(hr))
    {
        const std::wstring text = std::format(L"{} (0x{:08X})", LoadStringResource(IDS_ERR_RESIZE),
                                              static_cast<unsigned>(hr));
        MessageBoxW(text.c_str(), LoadStringResource(IDS_APP_TITLE).c_str(),
                    MB_OK | MB_ICONERROR);
        return;
    }

    // Baked like the other edits: transient until saved, kept out of the cache.
    m_cpuImage = std::move(resized);
    m_displayedPath.clear();
    m_tiles.clear();
    ApplyAutoZoomForNewImage();
    UpdateScrollBars();
    Invalidate(FALSE);
}

void MainWindow::ShowAboutBox()
{
    const std::wstring version = CHIRAMI_VERSION;
    const std::wstring text =
        std::vformat(LoadStringResource(IDS_ABOUT_TEXT), std::make_wformat_args(version));
    MessageBoxW(text.c_str(), LoadStringResource(IDS_ABOUT_TITLE).c_str(),
                MB_OK | MB_ICONINFORMATION);
}

LRESULT MainWindow::OnFileOpen(WORD, WORD, HWND, BOOL&)
{
    ShowFileOpenDialog();
    return 0;
}

LRESULT MainWindow::OnFileSaveAs(WORD, WORD, HWND, BOOL&)
{
    ShowFileSaveDialog();
    return 0;
}

LRESULT MainWindow::OnEditPaste(WORD, WORD, HWND, BOOL&)
{
    PasteFromClipboard();
    return 0;
}

LRESULT MainWindow::OnEditCrop(WORD, WORD, HWND, BOOL&)
{
    EnterSelectionMode(SelectionPurpose::Crop);
    return 0;
}

LRESULT MainWindow::OnEditBlackout(WORD, WORD, HWND, BOOL&)
{
    EnterSelectionMode(SelectionPurpose::Blackout);
    return 0;
}

LRESULT MainWindow::OnEditResize(WORD, WORD, HWND, BOOL&)
{
    ShowResizeDialog();
    return 0;
}

LRESULT MainWindow::OnEditTransform(WORD, WORD id, HWND, BOOL&)
{
    ApplyTransform(id);
    return 0;
}

LRESULT MainWindow::OnSortChanged(WORD, WORD id, HWND, BOOL&)
{
    switch (id)
    {
    case IDM_SORT_NAME:
        m_settings.sortKey = SortKey::Name;
        break;
    case IDM_SORT_DATE:
        m_settings.sortKey = SortKey::Date;
        break;
    case IDM_SORT_SIZE:
        m_settings.sortKey = SortKey::Size;
        break;
    case IDM_SORT_ASC:
        m_settings.sortDescending = false;
        break;
    case IDM_SORT_DESC:
        m_settings.sortDescending = true;
        break;
    default:
        return 0;
    }
    m_settings.Save();
    RescanFolder();  // the current file keeps its identity; the index follows
    return 0;
}

LRESULT MainWindow::OnFileExit(WORD, WORD, HWND, BOOL&)
{
    PostMessageW(WM_CLOSE);
    return 0;
}

LRESULT MainWindow::OnViewFit(WORD, WORD, HWND, BOOL&)
{
    SetZoomMode(ZoomMode::Fit);
    return 0;
}

LRESULT MainWindow::OnViewActual(WORD, WORD, HWND, BOOL&)
{
    SetZoomMode(ZoomMode::ActualSize);
    return 0;
}

LRESULT MainWindow::OnViewZoomIn(WORD, WORD, HWND, BOOL&)
{
    StepZoomAtCenter(+1);
    return 0;
}

LRESULT MainWindow::OnViewZoomOut(WORD, WORD, HWND, BOOL&)
{
    StepZoomAtCenter(-1);
    return 0;
}

LRESULT MainWindow::OnViewFullscreen(WORD, WORD, HWND, BOOL&)
{
    ToggleFullscreen();
    return 0;
}

LRESULT MainWindow::OnHelpAbout(WORD, WORD, HWND, BOOL&)
{
    ShowAboutBox();
    return 0;
}

// Re-evaluates the zoom (and the window size) whenever a new image arrives:
// fullscreen keeps fit display; otherwise, if the image fits the monitor's
// work area at 100% including the window chrome, the window shrinks/grows to
// exactly wrap the image and shows it dot-by-dot. If it doesn't fit, the
// window takes the largest image-shaped size the work area allows and the
// image is displayed fit.
void MainWindow::ApplyAutoZoomForNewImage()
{
    if (!m_cpuImage)
    {
        return;
    }

    if (m_fullscreen)
    {
        m_zoomMode = ZoomMode::Fit;
        return;  // never touch the window while fullscreen
    }

    // Maximized: keep the window as-is, only pick the zoom mode.
    if (IsZoomed())
    {
        CRect rc;
        GetClientRect(&rc);
        m_zoomMode = (static_cast<int>(m_cpuImage.width) <= rc.Width()
                      && static_cast<int>(m_cpuImage.height) <= rc.Height())
                         ? ZoomMode::ActualSize
                         : ZoomMode::Fit;
        return;
    }

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;

    // Chrome: borders, caption, and the menu bar (assumed single-row).
    // Scrollbars are excluded on purpose - the window is sized so that none
    // are needed.
    RECT frame{};
    AdjustWindowRectExForDpi(&frame, GetStyle() & ~(WS_HSCROLL | WS_VSCROLL),
                             GetMenu() != nullptr, GetExStyle(), m_dpi);
    const int chromeWidth = frame.right - frame.left;
    const int chromeHeight = frame.bottom - frame.top;

    const int imageWidth = static_cast<int>(m_cpuImage.width);
    const int imageHeight = static_cast<int>(m_cpuImage.height);

    int clientWidth;
    int clientHeight;
    if (imageWidth + chromeWidth <= workWidth && imageHeight + chromeHeight <= workHeight)
    {
        m_zoomMode = ZoomMode::ActualSize;
        clientWidth = imageWidth;
        clientHeight = imageHeight;
    }
    else
    {
        // Largest image-shaped client area the work area can hold; the long
        // edge of the image ends up flush with the work area.
        m_zoomMode = ZoomMode::Fit;
        const float scale =
            std::min(static_cast<float>(workWidth - chromeWidth) / imageWidth,
                     static_cast<float>(workHeight - chromeHeight) / imageHeight);
        clientWidth = static_cast<int>(std::round(imageWidth * scale));
        clientHeight = static_cast<int>(std::round(imageHeight * scale));
    }

    ResizeWindowToClient(clientWidth, clientHeight);
}

// After a zoom, size the window to the zoomed image: shrink-wrapped while it
// fits the work area, and grown to the largest size the work area allows
// (eventually covering it) with scrollbars once it doesn't.
void MainWindow::AutoFitWindowAfterZoom()
{
    if (!m_cpuImage || m_fullscreen || IsZoomed() || m_zoomMode == ZoomMode::Fit)
    {
        return;  // fit mode tracks the window by definition
    }

    const float scale = m_zoomMode == ZoomMode::ActualSize ? 1.0f : m_zoomScale;
    const int displayWidth =
        static_cast<int>(std::lround(static_cast<float>(m_cpuImage.width) * scale));
    const int displayHeight =
        static_cast<int>(std::lround(static_cast<float>(m_cpuImage.height) * scale));

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT frame{};
    AdjustWindowRectExForDpi(&frame, GetStyle() & ~(WS_HSCROLL | WS_VSCROLL),
                             GetMenu() != nullptr, GetExStyle(), m_dpi);
    const int maxClientWidth =
        (monitor.rcWork.right - monitor.rcWork.left) - (frame.right - frame.left);
    const int maxClientHeight =
        (monitor.rcWork.bottom - monitor.rcWork.top) - (frame.bottom - frame.top);
    const int barWidth = GetSystemMetricsForDpi(SM_CXVSCROLL, m_dpi);
    const int barHeight = GetSystemMetricsForDpi(SM_CYHSCROLL, m_dpi);

    // Which axes will overflow and need a scrollbar? A bar on one axis
    // steals viewport space from the other, hence the two passes.
    bool needH = false;
    bool needV = false;
    for (int pass = 0; pass < 2; ++pass)
    {
        needH = displayWidth > maxClientWidth - (needV ? barWidth : 0);
        needV = displayHeight > maxClientHeight - (needH ? barHeight : 0);
    }

    // Per axis: the viewport is the display size or whatever the work area
    // allows, plus the room the scrollbar itself takes.
    const int clientWidth =
        std::min(displayWidth, maxClientWidth - (needV ? barWidth : 0))
        + (needV ? barWidth : 0);
    const int clientHeight =
        std::min(displayHeight, maxClientHeight - (needH ? barHeight : 0))
        + (needH ? barHeight : 0);
    ResizeWindowToClient(clientWidth, clientHeight);
}

// Resizes the window so the client area (without scrollbars) has the given
// size, keeping the top-left corner as stationary as the work area allows
// and respecting the minimum tracking size.
void MainWindow::ResizeWindowToClient(int clientWidth, int clientHeight)
{
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;

    RECT frame{};
    AdjustWindowRectExForDpi(&frame, GetStyle() & ~(WS_HSCROLL | WS_VSCROLL),
                             GetMenu() != nullptr, GetExStyle(), m_dpi);

    int windowWidth = clientWidth + (frame.right - frame.left);
    int windowHeight = clientHeight + (frame.bottom - frame.top);
    windowWidth = std::max(windowWidth, GetSystemMetricsForDpi(SM_CXMINTRACK, m_dpi));
    windowHeight = std::max(windowHeight, GetSystemMetricsForDpi(SM_CYMINTRACK, m_dpi));

    // Keep the whole window inside the work area, moving it as little as
    // possible from where the user put it.
    const auto placeWindow = [&](int width, int height) {
        CRect windowRect;
        GetWindowRect(&windowRect);
        const int x = std::clamp(static_cast<int>(windowRect.left), static_cast<int>(work.left),
                                 std::max(static_cast<int>(work.left),
                                          static_cast<int>(work.right) - width));
        const int y = std::clamp(static_cast<int>(windowRect.top), static_cast<int>(work.top),
                                 std::max(static_cast<int>(work.top),
                                          static_cast<int>(work.bottom) - height));
        SetWindowPos(nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    placeWindow(windowWidth, windowHeight);

    // In a narrow window the menu bar can wrap onto several rows, which
    // AdjustWindowRectEx cannot predict (it assumes one row). Measure the
    // shortfall - ignoring any scrollbar that may have appeared meanwhile -
    // and grow the window once; the width doesn't change, so neither does
    // the wrapping.
    CRect client;
    GetClientRect(&client);
    int actualHeight = client.Height();
    if (GetStyle() & WS_HSCROLL)
    {
        actualHeight += GetSystemMetricsForDpi(SM_CYHSCROLL, m_dpi);
    }
    const int shortfall = clientHeight - actualHeight;
    if (shortfall > 0)
    {
        windowHeight =
            std::min(windowHeight + shortfall, static_cast<int>(work.bottom - work.top));
        placeWindow(windowWidth, windowHeight);
    }
}

void MainWindow::ToggleFullscreen()
{
    if (!m_fullscreen)
    {
        m_restorePlacement.length = sizeof(m_restorePlacement);
        GetWindowPlacement(&m_restorePlacement);
        m_fullscreen = true;

        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor);
        SetMenu(nullptr);  // m_menu keeps the handle for restoration
        ModifyStyle(WS_OVERLAPPEDWINDOW, 0);
        SetWindowPos(HWND_TOP, &monitor.rcMonitor, SWP_FRAMECHANGED);
    }
    else
    {
        m_fullscreen = false;
        SetMenu(m_menu);
        ModifyStyle(0, WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(&m_restorePlacement);  // restores maximized state too
        SetWindowPos(nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    UpdateScrollBars();
    Invalidate(FALSE);
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

    // Rounded to whole pixels so layout, scrollbar visibility, and the
    // window auto-fit all agree; raw float products carry rounding noise
    // (e.g. 640.00001) that would spuriously overflow an exactly-fitting
    // client area.
    layout.displayWidth = std::round(imageWidth * layout.scale);
    layout.displayHeight = std::round(imageHeight * layout.scale);
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
    int displayWidth = 0;
    int displayHeight = 0;

    // Fit mode never overflows the client area, so bars stay hidden.
    // Fullscreen suppresses them entirely; drag/wheel panning still works.
    if (m_cpuImage && m_zoomMode != ZoomMode::Fit && !m_fullscreen)
    {
        const float scale = m_zoomMode == ZoomMode::ActualSize ? 1.0f : m_zoomScale;
        // Whole pixels, same rounding as ComputeLayout/ResizeWindowToClient,
        // so an exactly-fitting window never shows bars from float noise.
        displayWidth = static_cast<int>(std::lround(m_cpuImage.width * scale));
        displayHeight = static_cast<int>(std::lround(m_cpuImage.height * scale));

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
            const int availWidth = grossWidth - (needV ? barWidth : 0);
            const int availHeight = grossHeight - (needH ? barHeight : 0);
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
    const ViewLayout layout = ComputeLayout();
    if (layout.scale <= 0.0f)
    {
        return;
    }
    const bool zoomingOut = newScale < layout.scale;
    newScale = std::clamp(newScale, kMinZoom, kMaxZoom);
    if (newScale == layout.scale)
    {
        MessageBeep(MB_OK);  // already at the zoom limit
        return;
    }
    if (zoomingOut)
    {
        // Refuse to keep zooming out once there is no room left to draw:
        // the client area may be gone entirely (minimum window size plus a
        // wrapped menu bar) or the image would shrink below a pixel.
        CRect rc;
        GetClientRect(&rc);
        const long newWidth = std::lround(m_cpuImage.width * newScale);
        const long newHeight = std::lround(m_cpuImage.height * newScale);
        if (rc.Width() <= 0 || rc.Height() <= 0 || newWidth < 1 || newHeight < 1)
        {
            MessageBeep(MB_OK);
            return;
        }
    }

    // Keep the image point under `anchor` stationary on screen.
    const float imageX = (static_cast<float>(anchor.x) - layout.destX) / layout.scale;
    const float imageY = (static_cast<float>(anchor.y) - layout.destY) / layout.scale;

    m_zoomMode = ZoomMode::Custom;
    m_zoomScale = newScale;
    m_panX = imageX * newScale - static_cast<float>(anchor.x);
    m_panY = imageY * newScale - static_cast<float>(anchor.y);

    AutoFitWindowAfterZoom();
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

void MainWindow::SetZoomMode(ZoomMode mode)
{
    if (!m_cpuImage || m_zoomMode == mode)
    {
        return;
    }
    m_zoomMode = mode;
    AutoFitWindowAfterZoom();
    UpdateScrollBars();

    // Start centered when the new mode overflows the client area.
    const ViewLayout layout = ComputeLayout();
    m_panX = layout.maxPanX / 2.0f;
    m_panY = layout.maxPanY / 2.0f;
    UpdateView();
}

void MainWindow::OnLButtonDown(UINT /*flags*/, CPoint point)
{
    if (InSelectionMode() && m_cpuImage)
    {
        const ViewLayout layout = ComputeLayout();
        if (layout.scale <= 0.0f)
        {
            return;
        }
        const D2D1_POINT_2F imagePoint = ClientToImage(point, layout);
        const SelectionHit hit =
            m_selection.HitTest(imagePoint, SelectionHitTolerance(layout));
        if (hit == SelectionHit::Inside)
        {
            // Arm the move but don't start it until the pointer passes the
            // drag threshold; a (double-)click must not nudge the selection.
            m_movePending = true;
            m_movePendingPoint = point;
            SetCapture();
            return;
        }
        m_selection.BeginDrag(hit, imagePoint);
        m_selection.UpdateDrag(imagePoint, static_cast<float>(m_cpuImage.width),
                               static_cast<float>(m_cpuImage.height));
        SetCapture();
        Invalidate(FALSE);
        return;
    }

    const ViewLayout layout = ComputeLayout();
    if (layout.maxPanX > 0.0f || layout.maxPanY > 0.0f)
    {
        m_dragging = true;
        m_dragLast = point;
        SetCapture();
    }
}

void MainWindow::OnLButtonDblClk(UINT flags, CPoint point)
{
    if (InSelectionMode() && m_cpuImage && m_selection.HasRect())
    {
        const ViewLayout layout = ComputeLayout();
        if (layout.scale > 0.0f
            && m_selection.HitTest(ClientToImage(point, layout), SelectionHitTolerance(layout))
                   == SelectionHit::Inside)
        {
            m_movePending = false;
            if (GetCapture() == m_hWnd)
            {
                ReleaseCapture();
            }
            ApplySelection();
            return;
        }
    }
    // The second press of a fast double-click elsewhere behaves like a
    // normal button-down.
    OnLButtonDown(flags, point);
}

void MainWindow::OnMouseMove(UINT /*flags*/, CPoint point)
{
    if (m_movePending && m_cpuImage)
    {
        if (std::abs(point.x - m_movePendingPoint.x)
                <= GetSystemMetricsForDpi(SM_CXDRAG, m_dpi)
            && std::abs(point.y - m_movePendingPoint.y)
                   <= GetSystemMetricsForDpi(SM_CYDRAG, m_dpi))
        {
            return;  // still within the click jitter zone
        }
        m_movePending = false;
        const ViewLayout layout = ComputeLayout();
        if (layout.scale <= 0.0f)
        {
            return;
        }
        m_selection.BeginDrag(SelectionHit::Inside,
                              ClientToImage(m_movePendingPoint, layout));
        // fall through to the drag update below
    }
    if (m_selection.Dragging() && m_cpuImage)
    {
        const ViewLayout layout = ComputeLayout();
        if (layout.scale > 0.0f)
        {
            m_selection.UpdateDrag(ClientToImage(point, layout),
                                   static_cast<float>(m_cpuImage.width),
                                   static_cast<float>(m_cpuImage.height));
            Invalidate(FALSE);
        }
        return;
    }
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
    if (m_movePending)
    {
        m_movePending = false;  // it was just a click; nothing moved
        ReleaseCapture();
        return;
    }
    if (m_selection.Dragging())
    {
        m_selection.EndDrag();
        ReleaseCapture();
        return;
    }
    if (m_dragging)
    {
        m_dragging = false;
        ReleaseCapture();
    }
}

void MainWindow::OnCaptureChanged(HWND /*newCapture*/)
{
    m_dragging = false;
    m_movePending = false;
    m_selection.EndDrag();
}

BOOL MainWindow::OnSetCursor(CWindow /*window*/, UINT hitTest, UINT /*message*/)
{
    if (hitTest == HTCLIENT && InSelectionMode() && m_cpuImage)
    {
        CPoint point;
        GetCursorPos(&point);
        ScreenToClient(&point);

        LPCWSTR cursor = IDC_CROSS;
        const ViewLayout layout = ComputeLayout();
        if (layout.scale > 0.0f)
        {
            switch (m_selection.HitTest(ClientToImage(point, layout),
                                        SelectionHitTolerance(layout)))
            {
            case SelectionHit::NW:
            case SelectionHit::SE:
                cursor = IDC_SIZENWSE;
                break;
            case SelectionHit::NE:
            case SelectionHit::SW:
                cursor = IDC_SIZENESW;
                break;
            case SelectionHit::E:
            case SelectionHit::W:
                cursor = IDC_SIZEWE;
                break;
            case SelectionHit::N:
            case SelectionHit::S:
                cursor = IDC_SIZENS;
                break;
            case SelectionHit::Inside:
                cursor = IDC_SIZEALL;
                break;
            case SelectionHit::None:
            default:
                break;
            }
        }
        SetCursor(LoadCursorW(nullptr, cursor));
        return TRUE;
    }
    SetMsgHandled(FALSE);
    return FALSE;
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
        Invalidate(FALSE);
    }
    else
    {
        DisplayImage(result->path.filename().wstring(), std::move(result->image));
    }
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

    if (m_openFirstAfterScan)
    {
        m_openFirstAfterScan = false;
        if (!m_folderFiles.empty())
        {
            LoadFile(m_folderFiles.front());
        }
        else
        {
            m_state = ViewState::Error;
            m_statusText = LoadStringResource(IDS_ERR_NO_IMAGES);
            Invalidate(FALSE);
        }
        return 0;
    }

    TriggerPrefetch();
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
    m_saver.reset();
    m_prefetcher.reset();
    // A menu attached to the window is destroyed with it; while fullscreen
    // it is detached and must be destroyed explicitly.
    if (m_fullscreen && !m_menu.IsNull())
    {
        m_menu.DestroyMenu();
    }
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
    m_tiles.clear();
    m_textBrush.reset();
    m_renderTarget.reset();
}

HRESULT MainWindow::CreateTilesFromCpuImage()
{
    m_tiles.clear();

    // Images beyond the GPU's maximum bitmap size are split into tiles.
    // 8192 keeps individual allocations moderate on any modern GPU.
    const uint32_t tileEdge = std::min(m_renderTarget->GetMaximumBitmapSize(), 8192u);
    RETURN_HR_IF(kHrImageTooLarge, tileEdge == 0);

    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    for (uint32_t y = 0; y < m_cpuImage.height; y += tileEdge)
    {
        for (uint32_t x = 0; x < m_cpuImage.width; x += tileEdge)
        {
            const uint32_t width = std::min(tileEdge, m_cpuImage.width - x);
            const uint32_t height = std::min(tileEdge, m_cpuImage.height - y);

            // 1px gutter of neighboring pixels (clamped to the image) so
            // linear sampling at the tile edges blends with real data
            // instead of clamping - otherwise the seams show when scaled.
            const uint32_t gutterLeft = x > 0 ? x - 1 : x;
            const uint32_t gutterTop = y > 0 ? y - 1 : y;
            const uint32_t gutterRight = std::min(m_cpuImage.width, x + width + 1);
            const uint32_t gutterBottom = std::min(m_cpuImage.height, y + height + 1);

            ImageTile tile;
            tile.source = D2D1::RectF(static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(x + width),
                                      static_cast<float>(y + height));
            tile.withGutter = D2D1::RectF(static_cast<float>(gutterLeft),
                                          static_cast<float>(gutterTop),
                                          static_cast<float>(gutterRight),
                                          static_cast<float>(gutterBottom));
            const HRESULT hr = m_renderTarget->CreateBitmap(
                D2D1::SizeU(gutterRight - gutterLeft, gutterBottom - gutterTop),
                m_cpuImage.pixels.data() + size_t{gutterTop} * m_cpuImage.stride
                    + size_t{gutterLeft} * 4,
                m_cpuImage.stride, properties, tile.bitmap.put());
            if (FAILED(hr))
            {
                m_tiles.clear();
                return hr;
            }
            m_tiles.push_back(std::move(tile));
        }
    }
    return S_OK;
}

void MainWindow::Render()
{
    if (FAILED(CreateDeviceResources()))
    {
        return;
    }

    if (m_tiles.empty() && m_cpuImage)
    {
        const HRESULT hr = CreateTilesFromCpuImage();
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
    if (!m_tiles.empty())
    {
        const ViewLayout layout = ComputeLayout();

        // Axis-aligned tiles must rasterize identically on both sides of a
        // shared edge. Per-primitive antialiasing would blend each tile's
        // fractional edge with the background separately, leaving a 1px
        // hairline at every boundary at non-integer zoom levels.
        const D2D1_ANTIALIAS_MODE previousMode = m_renderTarget->GetAntialiasMode();
        m_renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        for (const auto& tile : m_tiles)
        {
            // Shared edges compute from identical expressions on both sides
            // of a boundary, so adjacent tiles meet with no gap.
            const D2D1_RECT_F dest =
                D2D1::RectF(layout.destX + tile.source.left * layout.scale,
                            layout.destY + tile.source.top * layout.scale,
                            layout.destX + tile.source.right * layout.scale,
                            layout.destY + tile.source.bottom * layout.scale);
            const D2D1_RECT_F src =
                D2D1::RectF(tile.source.left - tile.withGutter.left,
                            tile.source.top - tile.withGutter.top,
                            tile.source.right - tile.withGutter.left,
                            tile.source.bottom - tile.withGutter.top);
            m_renderTarget->DrawBitmap(tile.bitmap.get(), dest, 1.0f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
        }
        m_renderTarget->SetAntialiasMode(previousMode);

        if (InSelectionMode() && m_selection.HasRect())
        {
            DrawSelectionOverlay(layout);
        }
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

void MainWindow::DrawSelectionOverlay(const ViewLayout& layout)
{
    const D2D1_RECT_F sel = m_selection.Rect();
    const D2D1_RECT_F rect = D2D1::RectF(
        layout.destX + sel.left * layout.scale, layout.destY + sel.top * layout.scale,
        layout.destX + sel.right * layout.scale, layout.destY + sel.bottom * layout.scale);
    const D2D1_SIZE_F client = m_renderTarget->GetSize();

    if (m_selectionPurpose == SelectionPurpose::Crop)
    {
        // Dim everything that would be cut away.
        m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black, 0.55f));
        m_renderTarget->FillRectangle(D2D1::RectF(0.0f, 0.0f, client.width, rect.top),
                                      m_textBrush.get());
        m_renderTarget->FillRectangle(D2D1::RectF(0.0f, rect.bottom, client.width, client.height),
                                      m_textBrush.get());
        m_renderTarget->FillRectangle(D2D1::RectF(0.0f, rect.top, rect.left, rect.bottom),
                                      m_textBrush.get());
        m_renderTarget->FillRectangle(
            D2D1::RectF(rect.right, rect.top, client.width, rect.bottom), m_textBrush.get());
    }
    else
    {
        // Preview of the redaction.
        m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black, 0.8f));
        m_renderTarget->FillRectangle(rect, m_textBrush.get());
    }

    // Rubber band: solid dark line with white dashes on top, visible over
    // any image content.
    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black));
    m_renderTarget->DrawRectangle(rect, m_textBrush.get(), 1.0f);
    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    m_renderTarget->DrawRectangle(rect, m_textBrush.get(), 1.0f, m_dashStroke.get());

    // 8 resize handles: corners and edge midpoints.
    const float half = 4.0f * DpiScale();
    const float midX = (rect.left + rect.right) / 2.0f;
    const float midY = (rect.top + rect.bottom) / 2.0f;
    const D2D1_POINT_2F handles[] = {
        {rect.left, rect.top},  {midX, rect.top},    {rect.right, rect.top},
        {rect.left, midY},                           {rect.right, midY},
        {rect.left, rect.bottom}, {midX, rect.bottom}, {rect.right, rect.bottom},
    };
    for (const auto& center : handles)
    {
        const D2D1_RECT_F handle =
            D2D1::RectF(center.x - half, center.y - half, center.x + half, center.y + half);
        m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        m_renderTarget->FillRectangle(handle, m_textBrush.get());
        m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black));
        m_renderTarget->DrawRectangle(handle, m_textBrush.get(), 1.0f);
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
