#include "MetadataWindow.h"
#include "resource.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace
{

constexpr int kDefaultWidthDip = 480;
constexpr int kDefaultHeightDip = 560;
constexpr int kMinWidthDip = 280;
constexpr int kMinHeightDip = 240;
constexpr int kNameColumnDip = 150;
constexpr int kDetailLines = 6;      // detail pane height, in text lines
constexpr size_t kMaxCellChars = 300;

constexpr UINT kCmdCopyValue = 1;
constexpr UINT kCmdCopyAll = 2;

std::wstring LoadStringResource(UINT id)
{
    WCHAR buffer[512];
    const int length = LoadStringW(_Module.GetResourceInstance(), id, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, static_cast<size_t>(std::max(length, 0)));
}

// First line of a value, clipped for the list cell; the full text lives in
// the detail pane.
std::wstring CellPreview(const std::wstring& value)
{
    size_t end = value.find_first_of(L"\r\n");
    bool clipped = end != std::wstring::npos;
    if (end == std::wstring::npos)
    {
        end = value.size();
    }
    if (end > kMaxCellChars)
    {
        end = kMaxCellChars;
        clipped = true;
    }
    std::wstring out = value.substr(0, end);
    if (clipped)
    {
        out += L" …";
    }
    return out;
}

// Edit controls need CRLF line breaks; values may carry LF or CRLF.
std::wstring ToEditText(const std::wstring& value)
{
    std::wstring out;
    out.reserve(value.size());
    for (const wchar_t c : value)
    {
        if (c == L'\r')
        {
            continue;
        }
        if (c == L'\n')
        {
            out += L"\r\n";
        }
        else
        {
            out += c;
        }
    }
    return out;
}

}  // namespace

void MetadataWindow::Open(HWND owner)
{
    if (IsWindow())
    {
        ShowWindow(SW_SHOWNOACTIVATE);
        return;
    }
    const std::wstring title = LoadStringResource(IDS_META_TITLE);
    static constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW & ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    if (Create(owner, rcDefault, title.c_str(), kStyle) == nullptr)
    {
        return;
    }
    if (m_lastPlacement.IsRectEmpty())
    {
        SetWindowPos(nullptr, 0, 0, MulDiv(kDefaultWidthDip, static_cast<int>(m_dpi), 96),
                     MulDiv(kDefaultHeightDip, static_cast<int>(m_dpi), 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CenterWindow(owner);
    }
    else
    {
        SetWindowPos(nullptr, &m_lastPlacement, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(SW_SHOWNOACTIVATE);
}

void MetadataWindow::SetItems(std::vector<MetadataItem> items)
{
    if (!IsWindow())
    {
        return;
    }
    m_items = std::move(items);
    Populate();
}

void MetadataWindow::SetError(HRESULT hr)
{
    if (!IsWindow())
    {
        return;
    }
    m_items.clear();
    Populate();
    m_detail.SetWindowTextW(std::format(L"{} (0x{:08X})", LoadStringResource(IDS_META_ERROR),
                                        static_cast<uint32_t>(hr))
                                .c_str());
}

BOOL MetadataWindow::PreTranslateMessage(MSG* msg)
{
    if (!IsWindow() || (msg->hwnd != m_hWnd && !IsChild(msg->hwnd)))
    {
        return FALSE;
    }
    if (msg->message == WM_KEYDOWN)
    {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const HWND focus = ::GetFocus();
        switch (msg->wParam)
        {
        case VK_ESCAPE:
            PostMessage(WM_CLOSE);
            return TRUE;
        case 'C':
            // The detail edit handles its own Ctrl+C (partial selection).
            if (control && focus == m_list.m_hWnd)
            {
                CopySelectedValues();
                return TRUE;
            }
            break;
        case 'A':
            if (control && focus == m_list.m_hWnd)
            {
                m_list.SetItemState(-1, LVIS_SELECTED, LVIS_SELECTED);
                return TRUE;
            }
            if (control && focus == m_detail.m_hWnd)
            {
                m_detail.SetSelAll();
                return TRUE;
            }
            break;
        default:
            break;
        }
    }
    // Tab moves between the list and the detail pane.
    return ::IsDialogMessageW(m_hWnd, msg);
}

int MetadataWindow::OnCreate(LPCREATESTRUCT /*createStruct*/)
{
    m_dpi = GetDpiForWindow(m_hWnd);

    m_list.Create(m_hWnd, rcDefault, nullptr,
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 0);
    m_list.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER
                                    | LVS_EX_LABELTIP);
    m_list.InsertColumn(0, LoadStringResource(IDS_META_COL_NAME).c_str(), LVCFMT_LEFT,
                        MulDiv(kNameColumnDip, static_cast<int>(m_dpi), 96));
    m_list.InsertColumn(1, LoadStringResource(IDS_META_COL_VALUE).c_str(), LVCFMT_LEFT, 100);
    m_list.EnableGroupView(TRUE);

    m_detail.Create(m_hWnd, rcDefault, nullptr,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE
                        | ES_READONLY | ES_AUTOVSCROLL,
                    WS_EX_CLIENTEDGE);

    UpdateFont();

    SetIcon(static_cast<HICON>(LoadImageW(
                _Module.GetResourceInstance(), MAKEINTRESOURCEW(IDI_CHIRAMI), IMAGE_ICON,
                GetSystemMetricsForDpi(SM_CXSMICON, m_dpi),
                GetSystemMetricsForDpi(SM_CYSMICON, m_dpi), 0)),
            FALSE);

    if (auto* loop = _Module.GetMessageLoop())
    {
        loop->AddMessageFilter(this);
    }
    return 0;
}

void MetadataWindow::OnSize(UINT /*type*/, CSize /*size*/)
{
    Layout();
}

void MetadataWindow::OnGetMinMaxInfo(LPMINMAXINFO info)
{
    info->ptMinTrackSize.x = MulDiv(kMinWidthDip, static_cast<int>(m_dpi), 96);
    info->ptMinTrackSize.y = MulDiv(kMinHeightDip, static_cast<int>(m_dpi), 96);
}

void MetadataWindow::OnSetFocus(CWindow /*lostFocus*/)
{
    m_list.SetFocus();
}

void MetadataWindow::OnContextMenu(CWindow window, CPoint point)
{
    if (window.m_hWnd != m_list.m_hWnd)
    {
        SetMsgHandled(FALSE);
        return;
    }
    if (point.x == -1 && point.y == -1)  // keyboard invocation
    {
        CRect rc;
        const int focused = m_list.GetNextItem(-1, LVNI_FOCUSED);
        if (focused >= 0 && m_list.GetItemRect(focused, &rc, LVIR_LABEL))
        {
            m_list.ClientToScreen(&rc);
            point = rc.CenterPoint();
        }
        else
        {
            m_list.GetWindowRect(&rc);
            point = rc.TopLeft();
        }
    }
    CMenu menu;
    menu.CreatePopupMenu();
    const bool hasSelection = m_list.GetSelectedCount() > 0;
    menu.AppendMenu(MF_STRING | (hasSelection ? MF_ENABLED : MF_GRAYED), kCmdCopyValue,
                    LoadStringResource(IDS_META_COPY_VALUE).c_str());
    menu.AppendMenu(MF_STRING | (!m_items.empty() ? MF_ENABLED : MF_GRAYED), kCmdCopyAll,
                    LoadStringResource(IDS_META_COPY_ALL).c_str());
    const UINT command = static_cast<UINT>(
        menu.TrackPopupMenu(TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, m_hWnd));
    if (command == kCmdCopyValue)
    {
        CopySelectedValues();
    }
    else if (command == kCmdCopyAll)
    {
        CopyAll();
    }
}

HBRUSH MetadataWindow::OnCtlColorStatic(CDCHandle dc, CStatic control)
{
    // The read-only detail edit asks through WM_CTLCOLORSTATIC; keep it on
    // the window background for readability.
    if (control.m_hWnd == m_detail.m_hWnd)
    {
        dc.SetTextColor(GetSysColor(COLOR_WINDOWTEXT));
        dc.SetBkColor(GetSysColor(COLOR_WINDOW));
        return GetSysColorBrush(COLOR_WINDOW);
    }
    SetMsgHandled(FALSE);
    return nullptr;
}

void MetadataWindow::OnDestroy()
{
    GetWindowRect(&m_lastPlacement);
    if (auto* loop = _Module.GetMessageLoop())
    {
        loop->RemoveMessageFilter(this);
    }
    SetMsgHandled(FALSE);
}

LRESULT MetadataWindow::OnDpiChanged(UINT /*msg*/, WPARAM wParam, LPARAM lParam,
                                     BOOL& /*handled*/)
{
    m_dpi = HIWORD(wParam);
    UpdateFont();
    if (auto* suggested = reinterpret_cast<RECT*>(lParam))
    {
        SetWindowPos(nullptr, suggested, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return 0;
}

LRESULT MetadataWindow::OnListItemChanged(int /*idCtrl*/, LPNMHDR header, BOOL& /*handled*/)
{
    const auto* change = reinterpret_cast<const NMLISTVIEW*>(header);
    if ((change->uChanged & LVIF_STATE) != 0)
    {
        UpdateDetail();
    }
    return 0;
}

void MetadataWindow::Populate()
{
    m_list.SetRedraw(FALSE);
    m_list.DeleteAllItems();
    m_list.RemoveAllGroups();

    struct GroupDef
    {
        MetadataGroup group;
        UINT titleId;
    };
    static constexpr GroupDef kGroups[] = {
        {MetadataGroup::File, IDS_META_GROUP_FILE},
        {MetadataGroup::Image, IDS_META_GROUP_IMAGE},
        {MetadataGroup::PngText, IDS_META_GROUP_PNGTEXT},
        {MetadataGroup::Wic, IDS_META_GROUP_WIC},
    };
    for (const auto& def : kGroups)
    {
        if (!std::ranges::any_of(m_items,
                                 [&](const auto& item) { return item.group == def.group; }))
        {
            continue;
        }
        std::wstring title = LoadStringResource(def.titleId);
        LVGROUP group{};
        group.cbSize = sizeof(group);
        group.mask = LVGF_HEADER | LVGF_GROUPID;
        group.pszHeader = title.data();
        group.iGroupId = static_cast<int>(def.group);
        m_list.InsertGroup(-1, &group);
    }

    int firstPngText = -1;
    for (size_t index = 0; index < m_items.size(); ++index)
    {
        const auto& item = m_items[index];
        std::wstring name = item.name;  // LVITEM::pszText is non-const
        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT | LVIF_GROUPID;
        lvi.iItem = static_cast<int>(index);
        lvi.pszText = name.data();
        lvi.iGroupId = static_cast<int>(item.group);
        m_list.InsertItem(&lvi);
        m_list.SetItemText(static_cast<int>(index), 1, CellPreview(item.value).c_str());
        if (firstPngText < 0 && item.group == MetadataGroup::PngText)
        {
            firstPngText = static_cast<int>(index);
        }
    }
    m_list.SetRedraw(TRUE);
    m_list.Invalidate();

    // Put the (likely) prompt straight into the detail pane; culling AI
    // outputs is this window's primary use.
    if (firstPngText >= 0)
    {
        m_list.SelectItem(firstPngText);
    }
    else
    {
        UpdateDetail();
    }
}

void MetadataWindow::Layout()
{
    if (!m_list.IsWindow())
    {
        return;
    }
    CRect client;
    GetClientRect(&client);

    int lineHeight = MulDiv(16, static_cast<int>(m_dpi), 96);
    if (m_font)
    {
        CClientDC dc(m_hWnd);
        const HFONT oldFont = dc.SelectFont(m_font.get());
        TEXTMETRICW tm{};
        if (dc.GetTextMetrics(&tm))
        {
            lineHeight = tm.tmHeight;
        }
        dc.SelectFont(oldFont);
    }
    const int detailHeight =
        std::min(lineHeight * kDetailLines + MulDiv(8, static_cast<int>(m_dpi), 96),
                 client.Height() / 2);

    m_list.MoveWindow(0, 0, client.Width(), client.Height() - detailHeight);
    m_detail.MoveWindow(0, client.Height() - detailHeight, client.Width(), detailHeight);

    // The value column fills whatever the name column leaves.
    CRect listClient;
    m_list.GetClientRect(&listClient);
    const int minValueWidth = MulDiv(80, static_cast<int>(m_dpi), 96);
    m_list.SetColumnWidth(1, std::max(listClient.Width() - m_list.GetColumnWidth(0),
                                      minValueWidth));
}

void MetadataWindow::UpdateFont()
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                    m_dpi)
        && !SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        return;
    }
    wil::unique_hfont font(CreateFontIndirectW(&metrics.lfMessageFont));
    if (font)
    {
        m_list.SetFont(font.get());
        m_detail.SetFont(font.get());
        m_font = std::move(font);
    }
}

void MetadataWindow::UpdateDetail()
{
    const int selected = m_list.GetNextItem(-1, LVNI_SELECTED);
    if (selected < 0 || static_cast<size_t>(selected) >= m_items.size())
    {
        m_detail.SetWindowTextW(L"");
        return;
    }
    m_detail.SetWindowTextW(ToEditText(m_items[static_cast<size_t>(selected)].value).c_str());
}

void MetadataWindow::CopySelectedValues()
{
    std::wstring text;
    for (int i = m_list.GetNextItem(-1, LVNI_SELECTED); i >= 0;
         i = m_list.GetNextItem(i, LVNI_SELECTED))
    {
        if (static_cast<size_t>(i) >= m_items.size())
        {
            continue;
        }
        if (!text.empty())
        {
            text += L"\r\n";
        }
        text += ToEditText(m_items[static_cast<size_t>(i)].value);
    }
    if (!text.empty())
    {
        CopyToClipboard(text);
    }
}

void MetadataWindow::CopyAll()
{
    std::wstring text;
    for (const auto& item : m_items)
    {
        text += item.name;
        text += L'\t';
        text += ToEditText(item.value);
        text += L"\r\n";
    }
    if (!text.empty())
    {
        CopyToClipboard(text);
    }
}

void MetadataWindow::CopyToClipboard(const std::wstring& text)
{
    if (!OpenClipboard())
    {
        return;
    }
    const auto close = wil::scope_exit([] { CloseClipboard(); });
    if (!EmptyClipboard())
    {
        return;
    }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    wil::unique_hglobal global(GlobalAlloc(GMEM_MOVEABLE, bytes));
    if (!global)
    {
        return;
    }
    void* buffer = GlobalLock(global.get());
    if (buffer == nullptr)
    {
        return;
    }
    std::memcpy(buffer, text.c_str(), bytes);
    GlobalUnlock(global.get());
    if (SetClipboardData(CF_UNICODETEXT, global.get()) != nullptr)
    {
        global.release();  // the clipboard owns the memory now
    }
}
