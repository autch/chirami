#pragma once

#include "framework.h"
#include "ImageMetadata.h"

#include <atlctrls.h>

#include <string>
#include <vector>

// Modeless "Image Properties" window: a grouped two-column list (property /
// value) over a read-only detail pane that shows the selected value in full,
// so long PNG text chunks (AI generation prompts) can be read and copied.
// Owned by the main window and updated as the displayed image changes.
class MetadataWindow : public CWindowImpl<MetadataWindow>, public CMessageFilter
{
public:
    DECLARE_WND_CLASS_EX(L"ChiramiMetadataWindow", 0, COLOR_WINDOW)

    // Creates (or re-shows) the window owned by `owner`. Shown without
    // activation so arrow-key navigation in the main window keeps working
    // while flipping through images.
    void Open(HWND owner);

    void SetItems(std::vector<MetadataItem> items);
    void SetError(HRESULT hr);

    BOOL PreTranslateMessage(MSG* msg) override;

    BEGIN_MSG_MAP(MetadataWindow)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_SIZE(OnSize)
        MSG_WM_GETMINMAXINFO(OnGetMinMaxInfo)
        MSG_WM_SETFOCUS(OnSetFocus)
        MSG_WM_CONTEXTMENU(OnContextMenu)
        MSG_WM_CTLCOLORSTATIC(OnCtlColorStatic)
        MSG_WM_DESTROY(OnDestroy)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        NOTIFY_CODE_HANDLER(LVN_ITEMCHANGED, OnListItemChanged)
    END_MSG_MAP()

private:
    int OnCreate(LPCREATESTRUCT createStruct);
    void OnSize(UINT type, CSize size);
    void OnGetMinMaxInfo(LPMINMAXINFO info);
    void OnSetFocus(CWindow lostFocus);
    void OnContextMenu(CWindow window, CPoint point);
    HBRUSH OnCtlColorStatic(CDCHandle dc, CStatic control);
    void OnDestroy();
    LRESULT OnDpiChanged(UINT msg, WPARAM wParam, LPARAM lParam, BOOL& handled);
    LRESULT OnListItemChanged(int idCtrl, LPNMHDR header, BOOL& handled);

    void Populate();
    void Layout();
    void UpdateFont();
    void UpdateDetail();
    void CopySelectedValues();
    void CopyAll();
    void CopyToClipboard(const std::wstring& text);

    CListViewCtrl m_list;
    CEdit m_detail;
    wil::unique_hfont m_font;
    std::vector<MetadataItem> m_items;  // full values; the list shows previews
    UINT m_dpi = USER_DEFAULT_SCREEN_DPI;
    CRect m_lastPlacement{};  // remembered across close/reopen this session
};
