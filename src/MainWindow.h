#pragma once

#include "framework.h"
#include "ImageLoader.h"
#include "LoadedImage.h"

#include <filesystem>
#include <memory>
#include <string>

inline constexpr UINT WM_APP_IMAGE_LOADED = WM_APP + 1;

class MainWindow : public CWindowImpl<MainWindow>
{
public:
    DECLARE_WND_CLASS_EX(L"ChiramiMainWindow", CS_HREDRAW | CS_VREDRAW, -1)

    BEGIN_MSG_MAP(MainWindow)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_SIZE(OnSize)
        MSG_WM_PAINT(OnPaint)
        MSG_WM_DESTROY(OnDestroy)
        MESSAGE_HANDLER(WM_APP_IMAGE_LOADED, OnImageLoaded)
    END_MSG_MAP()

    void LoadFile(std::filesystem::path path);

private:
    enum class ViewState
    {
        Empty,    // no file requested
        Loading,  // waiting for the loader
        Loaded,   // m_cpuImage valid, drawn from m_bitmap
        Error     // m_statusText holds the message
    };

    int OnCreate(LPCREATESTRUCT createStruct);
    void OnSize(UINT type, CSize size);
    void OnPaint(CDCHandle dc);
    void OnDestroy();
    LRESULT OnImageLoaded(UINT msg, WPARAM wParam, LPARAM lParam, BOOL& handled);

    HRESULT CreateDeviceResources();
    void DiscardDeviceResources();
    HRESULT CreateBitmapFromCpuImage();
    void Render();
    void DrawStatusText();
    void SetStatusError(HRESULT hr);

    static D2D1_RECT_F ComputeFitRect(D2D1_SIZE_F imageSize, D2D1_SIZE_F clientSize);

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
    LoadedImage m_cpuImage;  // kept after upload so device loss needs no re-decode
    ViewState m_state = ViewState::Empty;
    std::wstring m_statusText;
    uint64_t m_expectedGeneration = 0;
};
