#pragma once

#include "framework.h"
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

// Resize entry with four live-linked fields: width/height in pixels and in
// percent of the original size. With the aspect lock on (default), editing
// any one field updates the other three, so "give me 50%" and "width 1200,
// height follows" are both a single edit. The pixel fields are the source of
// truth at OK; the percent fields are input helpers.
class ResizeDialog : public CDialogImpl<ResizeDialog>
{
public:
    enum
    {
        IDD = IDD_RESIZE
    };

    // Far beyond any GPU limit; rejects absurd input before allocation.
    static constexpr uint32_t kMaxDimension = 32768;

    ResizeDialog(uint32_t width, uint32_t height)
        : m_originalWidth(width), m_originalHeight(height), m_width(width), m_height(height)
    {
    }

    uint32_t ResultWidth() const { return m_width; }
    uint32_t ResultHeight() const { return m_height; }

    BEGIN_MSG_MAP(ResizeDialog)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER(IDC_RESIZE_WIDTH, EN_CHANGE, OnFieldChanged)
        COMMAND_HANDLER(IDC_RESIZE_HEIGHT, EN_CHANGE, OnFieldChanged)
        COMMAND_HANDLER(IDC_RESIZE_WIDTH_PCT, EN_CHANGE, OnFieldChanged)
        COMMAND_HANDLER(IDC_RESIZE_HEIGHT_PCT, EN_CHANGE, OnFieldChanged)
        COMMAND_ID_HANDLER(IDOK, OnOk)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow /*focus*/, LPARAM /*param*/)
    {
        m_syncing = true;
        SetDlgItemInt(IDC_RESIZE_WIDTH, m_originalWidth, FALSE);
        SetDlgItemInt(IDC_RESIZE_HEIGHT, m_originalHeight, FALSE);
        SetPercent(IDC_RESIZE_WIDTH_PCT, 100.0);
        SetPercent(IDC_RESIZE_HEIGHT_PCT, 100.0);
        m_syncing = false;
        CheckDlgButton(IDC_RESIZE_KEEPASPECT, BST_CHECKED);
        CenterWindow(GetParent());
        return TRUE;
    }

    void SetPercent(int id, double value)
    {
        WCHAR buffer[32];
        swprintf_s(buffer, L"%.1f", value);
        SetDlgItemTextW(id, buffer);
    }

    double GetPercent(int id) const
    {
        WCHAR buffer[32]{};
        GetDlgItemTextW(id, buffer, ARRAYSIZE(buffer));
        WCHAR* end = nullptr;
        const double value = std::wcstod(buffer, &end);
        return end != buffer ? value : 0.0;
    }

    LRESULT OnFieldChanged(WORD /*code*/, WORD id, HWND /*control*/, BOOL& /*handled*/)
    {
        if (m_syncing)
        {
            return 0;
        }
        const bool keepAspect = IsDlgButtonChecked(IDC_RESIZE_KEEPASPECT) == BST_CHECKED;
        const double ratio =
            static_cast<double>(m_originalWidth) / static_cast<double>(m_originalHeight);

        // Derive the new pixel size from the edited field...
        BOOL valid = FALSE;
        long width = -1;   // -1: leave as currently entered
        long height = -1;
        switch (id)
        {
        case IDC_RESIZE_WIDTH:
        {
            const UINT value = GetDlgItemInt(IDC_RESIZE_WIDTH, &valid, FALSE);
            if (!valid || value == 0)
            {
                return 0;
            }
            width = value;
            if (keepAspect)
            {
                height = std::lround(value / ratio);
            }
            break;
        }
        case IDC_RESIZE_HEIGHT:
        {
            const UINT value = GetDlgItemInt(IDC_RESIZE_HEIGHT, &valid, FALSE);
            if (!valid || value == 0)
            {
                return 0;
            }
            height = value;
            if (keepAspect)
            {
                width = std::lround(value * ratio);
            }
            break;
        }
        case IDC_RESIZE_WIDTH_PCT:
        {
            const double percent = GetPercent(IDC_RESIZE_WIDTH_PCT);
            if (percent <= 0.0)
            {
                return 0;
            }
            width = std::lround(m_originalWidth * percent / 100.0);
            if (keepAspect)
            {
                height = std::lround(m_originalHeight * percent / 100.0);
            }
            break;
        }
        case IDC_RESIZE_HEIGHT_PCT:
        {
            const double percent = GetPercent(IDC_RESIZE_HEIGHT_PCT);
            if (percent <= 0.0)
            {
                return 0;
            }
            height = std::lround(m_originalHeight * percent / 100.0);
            if (keepAspect)
            {
                width = std::lround(m_originalWidth * percent / 100.0);
            }
            break;
        }
        default:
            return 0;
        }

        // ...and update every field except the one being typed into.
        m_syncing = true;
        if (width > 0 && id != IDC_RESIZE_WIDTH)
        {
            SetDlgItemInt(IDC_RESIZE_WIDTH, static_cast<UINT>(std::max(1L, width)), FALSE);
        }
        if (height > 0 && id != IDC_RESIZE_HEIGHT)
        {
            SetDlgItemInt(IDC_RESIZE_HEIGHT, static_cast<UINT>(std::max(1L, height)), FALSE);
        }
        if (width > 0 && id != IDC_RESIZE_WIDTH_PCT)
        {
            SetPercent(IDC_RESIZE_WIDTH_PCT, width * 100.0 / m_originalWidth);
        }
        if (height > 0 && id != IDC_RESIZE_HEIGHT_PCT)
        {
            SetPercent(IDC_RESIZE_HEIGHT_PCT, height * 100.0 / m_originalHeight);
        }
        m_syncing = false;
        return 0;
    }

    LRESULT OnOk(WORD, WORD, HWND, BOOL&)
    {
        BOOL widthValid = FALSE;
        BOOL heightValid = FALSE;
        const UINT width = GetDlgItemInt(IDC_RESIZE_WIDTH, &widthValid, FALSE);
        const UINT height = GetDlgItemInt(IDC_RESIZE_HEIGHT, &heightValid, FALSE);
        if (!widthValid || !heightValid || width == 0 || height == 0
            || width > kMaxDimension || height > kMaxDimension)
        {
            MessageBeep(MB_OK);
            return 0;
        }
        m_width = width;
        m_height = height;
        EndDialog(IDOK);
        return 0;
    }

    LRESULT OnCancel(WORD, WORD, HWND, BOOL&)
    {
        EndDialog(IDCANCEL);
        return 0;
    }

    uint32_t m_originalWidth;
    uint32_t m_originalHeight;
    uint32_t m_width;
    uint32_t m_height;
    bool m_syncing = false;
};
