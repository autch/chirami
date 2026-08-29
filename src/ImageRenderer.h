#pragma once

#include "framework.h"
#include "LoadedImage.h"
#include "ViewLayout.h"

#include <vector>

// D3D11 + Direct2D swap-chain renderer (scRGB, tiled uploads, SDR white-level
// boost). Device-independent factories stay with the window; this owns the
// device-dependent resources and presents one frame at a time.
class ImageRenderer
{
public:
    ImageRenderer(HWND hwnd, ID2D1Factory1* factory);
    ~ImageRenderer();

    ImageRenderer(const ImageRenderer&) = delete;
    ImageRenderer& operator=(const ImageRenderer&) = delete;

    HRESULT EnsureDevice();
    void DiscardDevice();
    bool HasDevice() const { return m_d2dContext != nullptr; }

    // No-op until the first EnsureDevice. Device-removed HRESULTs are
    // returned as-is so the caller can DiscardDevice.
    HRESULT Resize(UINT width, UINT height);

    // Queries DISPLAYCONFIG_SDR_WHITE_LEVEL for the monitor hwnd sits on.
    // Returns true when the boost changed (the caller should repaint).
    bool UpdateSdrWhiteLevel();

    void ClearTiles();
    bool HasTiles() const { return !m_tiles.empty(); }
    HRESULT UploadImage(const LoadedImage& image);

    struct Overlay
    {
        const wchar_t* statusText = nullptr;
        UINT32 statusLength = 0;
        IDWriteTextFormat* textFormat = nullptr;
        float dpiScale = 1.0f;

        bool showSelection = false;
        bool cropSelection = false;  // dim outside; otherwise blackout preview
        D2D1_RECT_F selection{};     // image coordinates
    };

    // Draws tiles at `layout` when both are present, then overlays, then
    // presents. Device-loss HRESULTs are returned to the caller.
    HRESULT Present(D2D1_COLOR_F background, const ViewLayout* layout, const Overlay& overlay);

    static bool IsDeviceLost(HRESULT hr)
    {
        return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED
               || hr == DXGI_ERROR_DEVICE_RESET;
    }

private:
    HRESULT CreateTargetBitmap();
    void DrawTiles(const ViewLayout& layout);
    void DrawSelectionOverlay(const ViewLayout& layout, const Overlay& overlay);
    void DrawStatusText(const Overlay& overlay);

    struct ImageTile
    {
        D2D1_RECT_F source{};      // image region this tile displays
        D2D1_RECT_F withGutter{};  // image region the bitmap actually holds
        wil::com_ptr<ID2D1Bitmap> bitmap;
    };

    HWND m_hwnd = nullptr;
    ID2D1Factory1* m_factory = nullptr;  // not owned; window lifetime
    wil::com_ptr<ID2D1StrokeStyle> m_dashStroke;

    wil::com_ptr<ID3D11Device> m_d3dDevice;
    wil::com_ptr<IDXGISwapChain1> m_swapChain;
    wil::com_ptr<ID2D1Device> m_d2dDevice;
    wil::com_ptr<ID2D1DeviceContext> m_d2dContext;
    wil::com_ptr<ID2D1Bitmap1> m_targetBitmap;
    wil::com_ptr<ID2D1Bitmap1> m_sceneBitmap;      // used when m_sdrBoost > 1
    wil::com_ptr<ID2D1Effect> m_whiteLevelEffect;  // ColorMatrix scaling by m_sdrBoost
    wil::com_ptr<ID2D1SolidColorBrush> m_textBrush;
    std::vector<ImageTile> m_tiles;

    // With HDR (advanced color) enabled, DWM boosts ordinary SDR windows to
    // the user's SDR white level but composes scRGB surfaces at 1.0 == 80
    // nits. Scaling the whole scene by this factor keeps chirami's SDR
    // brightness in line with every other window; HDR pixels get the same
    // headroom above it. 1.0 on SDR displays.
    float m_sdrBoost = 1.0f;
};
