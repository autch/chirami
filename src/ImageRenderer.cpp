#include "ImageRenderer.h"

#include <d2d1effects.h>  // CLSID_D2D1ColorMatrix

#include <algorithm>
#include <utility>
#include <vector>

ImageRenderer::ImageRenderer(HWND hwnd, ID2D1Factory1* factory) : m_hwnd(hwnd), m_factory(factory)
{
    THROW_IF_FAILED(m_factory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                    D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f),
        nullptr, 0, m_dashStroke.put()));
}

ImageRenderer::~ImageRenderer()
{
    DiscardDevice();
}

HRESULT ImageRenderer::EnsureDevice()
{
    if (!m_d2dContext)
    {
        // D3D11 device with BGRA support for D2D interop; WARP keeps the
        // viewer working without functional GPU drivers.
        wil::com_ptr<ID3D11Device> device;
        constexpr D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_1,
        };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED, kLevels,
            ARRAYSIZE(kLevels), D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED, kLevels,
                ARRAYSIZE(kLevels), D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
        }
        RETURN_IF_FAILED(hr);

        const auto dxgiDevice = device.query<IDXGIDevice>();
        wil::com_ptr<IDXGIAdapter> adapter;
        RETURN_IF_FAILED(dxgiDevice->GetAdapter(adapter.put()));
        wil::com_ptr<IDXGIFactory2> dxgiFactory;
        RETURN_IF_FAILED(adapter->GetParent(IID_PPV_ARGS(dxgiFactory.put())));

        // FP16 flip-model swap chain in scRGB (linear, 1.0 == SDR white):
        // correct on SDR displays and HDR-capable on advanced-color ones.
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(std::max(1L, rc.right - rc.left));
        desc.Height = static_cast<UINT>(std::max(1L, rc.bottom - rc.top));
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc = {1, 0};
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Scaling = DXGI_SCALING_NONE;
        wil::com_ptr<IDXGISwapChain1> swapChain;
        RETURN_IF_FAILED(dxgiFactory->CreateSwapChainForHwnd(device.get(), m_hwnd, &desc, nullptr,
                                                             nullptr, swapChain.put()));
        if (const auto swapChain3 = swapChain.try_query<IDXGISwapChain3>())
        {
            constexpr DXGI_COLOR_SPACE_TYPE kScRgb = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            UINT support = 0;
            if (SUCCEEDED(swapChain3->CheckColorSpaceSupport(kScRgb, &support))
                && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
            {
                (void)swapChain3->SetColorSpace1(kScRgb);
            }
        }
        (void)dxgiFactory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

        wil::com_ptr<ID2D1Device> d2dDevice;
        RETURN_IF_FAILED(m_factory->CreateDevice(dxgiDevice.get(), d2dDevice.put()));
        wil::com_ptr<ID2D1DeviceContext> context;
        RETURN_IF_FAILED(
            d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.put()));
        context->SetDpi(96.0f, 96.0f);  // 1 DIP == 1 physical pixel, as before

        wil::com_ptr<ID2D1Effect> whiteLevel;
        RETURN_IF_FAILED(context->CreateEffect(CLSID_D2D1ColorMatrix, whiteLevel.put()));

        m_d3dDevice = std::move(device);
        m_swapChain = std::move(swapChain);
        m_d2dDevice = std::move(d2dDevice);
        m_d2dContext = std::move(context);
        m_whiteLevelEffect = std::move(whiteLevel);
        (void)UpdateSdrWhiteLevel();
    }
    if (!m_targetBitmap)
    {
        RETURN_IF_FAILED(CreateTargetBitmap());
    }
    if (!m_textBrush)
    {
        RETURN_IF_FAILED(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                                             m_textBrush.put()));
    }
    return S_OK;
}

HRESULT ImageRenderer::Resize(UINT width, UINT height)
{
    if (!m_d2dContext)
    {
        return S_OK;
    }
    // The target bitmap holds a swap chain buffer reference and must go
    // before ResizeBuffers; the image tiles live on the D2D device and
    // survive, so no re-upload happens on resize.
    m_d2dContext->SetTarget(nullptr);
    m_targetBitmap.reset();
    m_sceneBitmap.reset();
    const HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
        return hr;
    }
    return CreateTargetBitmap();
}

HRESULT ImageRenderer::CreateTargetBitmap()
{
    wil::com_ptr<IDXGISurface> surface;
    RETURN_IF_FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(surface.put())));
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    wil::com_ptr<ID2D1Bitmap1> bitmap;
    RETURN_IF_FAILED(
        m_d2dContext->CreateBitmapFromDxgiSurface(surface.get(), &properties, bitmap.put()));
    m_d2dContext->SetTarget(bitmap.get());
    m_targetBitmap = std::move(bitmap);

    // Same-sized intermediate for the SDR white-level pass (drawable, so no
    // CANNOT_DRAW). Kept in lockstep with the back buffer size.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    RETURN_IF_FAILED(m_swapChain->GetDesc1(&desc));
    const D2D1_BITMAP_PROPERTIES1 sceneProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
        96.0f);
    m_sceneBitmap.reset();
    RETURN_IF_FAILED(m_d2dContext->CreateBitmap(D2D1::SizeU(desc.Width, desc.Height), nullptr, 0,
                                                sceneProperties, m_sceneBitmap.put()));
    return S_OK;
}

bool ImageRenderer::UpdateSdrWhiteLevel()
{
    float boost = 1.0f;

    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)
            == ERROR_SUCCESS)
        {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                   modes.data(), nullptr)
                == ERROR_SUCCESS)
            {
                for (UINT32 i = 0; i < pathCount; ++i)
                {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    source.header.size = sizeof(source);
                    source.header.adapterId = paths[i].sourceInfo.adapterId;
                    source.header.id = paths[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
                        || wcscmp(source.viewGdiDeviceName, monitor.szDevice) != 0)
                    {
                        continue;
                    }
                    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
                    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                    white.header.size = sizeof(white);
                    white.header.adapterId = paths[i].targetInfo.adapterId;
                    white.header.id = paths[i].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS
                        && white.SDRWhiteLevel >= 1000)
                    {
                        boost = static_cast<float>(white.SDRWhiteLevel) / 1000.0f;
                    }
                    break;
                }
            }
        }
    }

    if (boost == m_sdrBoost)
    {
        return false;
    }
    m_sdrBoost = boost;
    return true;
}

void ImageRenderer::DiscardDevice()
{
    m_tiles.clear();
    m_textBrush.reset();
    if (m_d2dContext)
    {
        m_d2dContext->SetTarget(nullptr);
    }
    m_whiteLevelEffect.reset();
    m_sceneBitmap.reset();
    m_targetBitmap.reset();
    m_d2dContext.reset();
    m_d2dDevice.reset();
    m_swapChain.reset();
    m_d3dDevice.reset();
}

void ImageRenderer::ClearTiles()
{
    m_tiles.clear();
}

HRESULT ImageRenderer::UploadImage(const LoadedImage& image)
{
    m_tiles.clear();

    // Images beyond the GPU's maximum bitmap size are split into tiles.
    // 8192 keeps individual allocations moderate on any modern GPU.
    const uint32_t tileEdge = std::min(m_d2dContext->GetMaximumBitmapSize(), 8192u);
    RETURN_HR_IF(kHrImageTooLarge, tileEdge == 0);

    // SDR tiles use _SRGB so the hardware linearizes the 8-bit pixels when
    // sampling - exactly the conversion the linear-gamma (scRGB) target
    // needs. HDR tiles are half floats already in scRGB.
    const bool halfFloat = image.format == LoadedImage::Format::Rgba16F;
    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(halfFloat ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                    : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                          D2D1_ALPHA_MODE_PREMULTIPLIED));
    const uint32_t bytesPerPixel = image.BytesPerPixel();

    for (uint32_t y = 0; y < image.height; y += tileEdge)
    {
        for (uint32_t x = 0; x < image.width; x += tileEdge)
        {
            const uint32_t width = std::min(tileEdge, image.width - x);
            const uint32_t height = std::min(tileEdge, image.height - y);

            // 1px gutter of neighboring pixels (clamped to the image) so
            // linear sampling at the tile edges blends with real data
            // instead of clamping - otherwise the seams show when scaled.
            const uint32_t gutterLeft = x > 0 ? x - 1 : x;
            const uint32_t gutterTop = y > 0 ? y - 1 : y;
            const uint32_t gutterRight = std::min(image.width, x + width + 1);
            const uint32_t gutterBottom = std::min(image.height, y + height + 1);

            ImageTile tile;
            tile.source = D2D1::RectF(static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(x + width),
                                      static_cast<float>(y + height));
            tile.withGutter =
                D2D1::RectF(static_cast<float>(gutterLeft), static_cast<float>(gutterTop),
                            static_cast<float>(gutterRight), static_cast<float>(gutterBottom));
            const HRESULT hr = m_d2dContext->CreateBitmap(
                D2D1::SizeU(gutterRight - gutterLeft, gutterBottom - gutterTop),
                image.pixels.data() + size_t{gutterTop} * image.stride
                    + size_t{gutterLeft} * bytesPerPixel,
                image.stride, properties, tile.bitmap.put());
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

HRESULT ImageRenderer::Present(D2D1_COLOR_F background, const ViewLayout* layout,
                               const Overlay& overlay)
{
    // With an SDR white boost, the scene draws into an intermediate bitmap
    // and a ColorMatrix multiply lifts it onto the back buffer, so SDR
    // content matches the brightness of every other window under HDR.
    const bool applyBoost = m_sdrBoost > 1.001f && m_sceneBitmap && m_whiteLevelEffect;
    m_d2dContext->SetTarget(applyBoost ? m_sceneBitmap.get() : m_targetBitmap.get());

    m_d2dContext->BeginDraw();
    m_d2dContext->Clear(background);

    if (layout && !m_tiles.empty())
    {
        DrawTiles(*layout);
        if (overlay.showSelection)
        {
            DrawSelectionOverlay(*layout, overlay);
        }
    }
    if (overlay.statusText != nullptr && overlay.statusLength > 0 && overlay.textFormat != nullptr)
    {
        DrawStatusText(overlay);
    }

    if (applyBoost)
    {
        m_d2dContext->SetTarget(m_targetBitmap.get());
        const float boost = m_sdrBoost;
        const D2D1_MATRIX_5X4_F matrix = {
            boost, 0, 0, 0,  //
            0, boost, 0, 0,  //
            0, 0, boost, 0,  //
            0, 0, 0, 1,      //
            0, 0, 0, 0,      //
        };
        m_whiteLevelEffect->SetInput(0, m_sceneBitmap.get());
        (void)m_whiteLevelEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix);
        m_d2dContext->DrawImage(m_whiteLevelEffect.get());  // 1:1, no transform
    }

    HRESULT hr = m_d2dContext->EndDraw();
    if (SUCCEEDED(hr))
    {
        hr = m_swapChain->Present(1, 0);
    }
    return hr;
}

void ImageRenderer::DrawTiles(const ViewLayout& layout)
{
    // Axis-aligned tiles must rasterize identically on both sides of a
    // shared edge. Per-primitive antialiasing would blend each tile's
    // fractional edge with the background separately, leaving a 1px
    // hairline at every boundary at non-integer zoom levels.
    const D2D1_ANTIALIAS_MODE previousMode = m_d2dContext->GetAntialiasMode();
    m_d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
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
        m_d2dContext->DrawBitmap(tile.bitmap.get(), dest, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    }
    m_d2dContext->SetAntialiasMode(previousMode);
}

void ImageRenderer::DrawSelectionOverlay(const ViewLayout& layout, const Overlay& overlay)
{
    const D2D1_RECT_F sel = overlay.selection;
    const D2D1_RECT_F rect =
        D2D1::RectF(layout.destX + sel.left * layout.scale, layout.destY + sel.top * layout.scale,
                    layout.destX + sel.right * layout.scale,
                    layout.destY + sel.bottom * layout.scale);
    const D2D1_SIZE_F client = m_d2dContext->GetSize();

    if (overlay.cropSelection)
    {
        // Dim everything that would be cut away.
        m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black, 0.55f));
        m_d2dContext->FillRectangle(D2D1::RectF(0.0f, 0.0f, client.width, rect.top),
                                    m_textBrush.get());
        m_d2dContext->FillRectangle(D2D1::RectF(0.0f, rect.bottom, client.width, client.height),
                                    m_textBrush.get());
        m_d2dContext->FillRectangle(D2D1::RectF(0.0f, rect.top, rect.left, rect.bottom),
                                    m_textBrush.get());
        m_d2dContext->FillRectangle(D2D1::RectF(rect.right, rect.top, client.width, rect.bottom),
                                    m_textBrush.get());
    }
    else
    {
        // Preview of the redaction.
        m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black, 0.8f));
        m_d2dContext->FillRectangle(rect, m_textBrush.get());
    }

    // Rubber band: solid dark line with white dashes on top, visible over
    // any image content.
    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black));
    m_d2dContext->DrawRectangle(rect, m_textBrush.get(), 1.0f);
    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    m_d2dContext->DrawRectangle(rect, m_textBrush.get(), 1.0f, m_dashStroke.get());

    // 8 resize handles: corners and edge midpoints.
    const float half = 4.0f * overlay.dpiScale;
    const float midX = (rect.left + rect.right) / 2.0f;
    const float midY = (rect.top + rect.bottom) / 2.0f;
    const D2D1_POINT_2F handles[] = {
        {rect.left, rect.top},     {midX, rect.top},      {rect.right, rect.top},
        {rect.left, midY},                                {rect.right, midY},
        {rect.left, rect.bottom},  {midX, rect.bottom},   {rect.right, rect.bottom},
    };
    for (const auto& center : handles)
    {
        const D2D1_RECT_F handle =
            D2D1::RectF(center.x - half, center.y - half, center.x + half, center.y + half);
        m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        m_d2dContext->FillRectangle(handle, m_textBrush.get());
        m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black));
        m_d2dContext->DrawRectangle(handle, m_textBrush.get(), 1.0f);
    }
}

void ImageRenderer::DrawStatusText(const Overlay& overlay)
{
    const D2D1_SIZE_F size = m_d2dContext->GetSize();

    // Semi-transparent backplate keeps the text readable over any image.
    const float halfBandHeight = 24.0f * overlay.dpiScale;
    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black, 0.6f));
    m_d2dContext->FillRectangle(
        D2D1::RectF(0.0f, size.height / 2.0f - halfBandHeight, size.width,
                    size.height / 2.0f + halfBandHeight),
        m_textBrush.get());

    m_textBrush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    m_d2dContext->DrawText(overlay.statusText, overlay.statusLength, overlay.textFormat,
                           D2D1::RectF(0.0f, 0.0f, size.width, size.height), m_textBrush.get());
}
