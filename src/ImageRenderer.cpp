#include "ImageRenderer.h"

#include "GainMapMath.h"

#include <d2d1effects.h>  // CLSID_D2D1ColorMatrix, TableTransfer, ...
#include <dxgi1_6.h>      // IDXGIOutput6 / DXGI_OUTPUT_DESC1

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace
{

// Entries in the gain-map lookup table; the effect interpolates between them.
constexpr size_t kGainTableSize = 256;

// Windows composes scRGB at 1.0 == 80 nits (D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL).
constexpr float kScRgbWhiteNits = 80.0f;

}  // namespace

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
        (void)UpdateDisplayLevels(true);
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

bool ImageRenderer::UpdateDisplayLevels(bool displayChanged)
{
    const float boost = QuerySdrBoost();
    const bool boostChanged = boost != m_sdrBoost;
    m_sdrBoost = boost;  // QueryDisplayHeadroom measures against it

    const HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    if (!displayChanged && !boostChanged && monitor == m_headroomMonitor)
    {
        return false;
    }
    m_headroomMonitor = monitor;
    const float headroom = QueryDisplayHeadroom();
    const bool headroomChanged = headroom != m_displayHeadroom;
    if (headroomChanged)
    {
        m_displayHeadroom = headroom;
        UpdateGainEffects();
    }
    return boostChanged || headroomChanged;
}

float ImageRenderer::QuerySdrBoost() const
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

    return boost;
}

// Microsoft's advice for desktop apps: find the DXGI output the window
// overlaps most (GetContainingOutput can return a stale one) and read its
// capabilities from DXGI_OUTPUT_DESC1. A fresh factory each time, since one
// created before a display change enumerates stale outputs.
float ImageRenderer::QueryDisplayHeadroom() const
{
    RECT window{};
    if (!GetWindowRect(m_hwnd, &window))
    {
        return 1.0f;
    }
    wil::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
    {
        return 1.0f;
    }

    DXGI_OUTPUT_DESC1 best{};
    long bestArea = -1;
    wil::com_ptr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        wil::com_ptr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            const auto output6 = output.try_query<IDXGIOutput6>();
            DXGI_OUTPUT_DESC1 desc{};
            if (!output6 || FAILED(output6->GetDesc1(&desc)))
            {
                continue;
            }
            RECT overlap{};
            const long area = IntersectRect(&overlap, &window, &desc.DesktopCoordinates)
                                  ? (overlap.right - overlap.left) * (overlap.bottom - overlap.top)
                                  : 0;
            if (area > bestArea)
            {
                bestArea = area;
                best = desc;
            }
        }
    }

    // Only an HDR (PQ) output reproduces anything above SDR white.
    if (bestArea < 0 || best.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
    {
        return 1.0f;
    }
    const float sdrWhiteNits = kScRgbWhiteNits * m_sdrBoost;
    return std::max(best.MaxLuminance / sdrWhiteNits, 1.0f);
}

void ImageRenderer::DiscardDevice()
{
    m_tiles.clear();
    m_gainBitmap.reset();
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
    m_gainBitmap.reset();
}

HRESULT ImageRenderer::UploadImage(const LoadedImage& image)
{
    ClearTiles();

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

    // The gain map uploads as plain UNORM (it is not sRGB-encoded) and must
    // fit in one bitmap. One that does not - never seen in practice, maps
    // are about half the base - just leaves the image SDR.
    const PixelBuffer& map = image.gainMap.map;
    if (image.gainMap && map.format == PixelBuffer::Format::Bgra8 && map.width <= tileEdge
        && map.height <= tileEdge)
    {
        const HRESULT hr = m_d2dContext->CreateBitmap(
            D2D1::SizeU(map.width, map.height), map.pixels.data(), map.stride,
            D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            m_gainBitmap.put());
        if (FAILED(hr))
        {
            LOG_HR(hr);
            m_gainBitmap.reset();
        }
        m_contentHeadroom = image.gainMap.headroom;
        m_toSrgb = image.toSrgb.value_or(ColorMatrix3{1, 0, 0, 0, 1, 0, 0, 0, 1});
    }

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
                ClearTiles();
                return hr;
            }
            if (m_gainBitmap)
            {
                if (const HRESULT gainHr = CreateGainEffects(tile, image); FAILED(gainHr))
                {
                    // Show the SDR base rather than nothing.
                    LOG_HR(gainHr);
                    tile.gainTable.reset();
                    tile.colorMatrix.reset();
                }
            }
            m_tiles.push_back(std::move(tile));
        }
    }
    UpdateGainEffects();
    return S_OK;
}

// Per tile: the gain map, stretched over the whole image and shifted into
// this tile's bitmap coordinates, goes through a lookup table (gain ->
// boost / divisor), multiplies the linear base pixels, and a color matrix
// multiplies the divisor back in while converting the base's primaries to
// sRGB. The table and the matrix depend on the display and are filled in
// by UpdateGainEffects. See DESIGN.md, "HDR gain map".
HRESULT ImageRenderer::CreateGainEffects(ImageTile& tile, const LoadedImage& image)
{
    const D2D1_SIZE_U mapSize = m_gainBitmap->GetPixelSize();

    wil::com_ptr<ID2D1Effect> stretch;
    RETURN_IF_FAILED(m_d2dContext->CreateEffect(CLSID_D2D12DAffineTransform, stretch.put()));
    stretch->SetInput(0, m_gainBitmap.get());
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Scale(static_cast<float>(image.width) / mapSize.width,
                                static_cast<float>(image.height) / mapSize.height)
        * D2D1::Matrix3x2F::Translation(-tile.withGutter.left, -tile.withGutter.top);
    RETURN_IF_FAILED(stretch->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, transform));
    RETURN_IF_FAILED(stretch->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                                       D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR));
    // Hard edges: a soft border would fade the gain to zero at the image edge.
    RETURN_IF_FAILED(stretch->SetValue(D2D1_2DAFFINETRANSFORM_PROP_BORDER_MODE,
                                       D2D1_BORDER_MODE_HARD));

    wil::com_ptr<ID2D1Effect> table;
    RETURN_IF_FAILED(m_d2dContext->CreateEffect(CLSID_D2D1TableTransfer, table.put()));
    table->SetInputEffect(0, stretch.get());
    RETURN_IF_FAILED(table->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_DISABLE, TRUE));

    // Output = A * input0 * input1 (B, C, D zero). Both inputs are
    // premultiplied and the map is opaque, so alpha passes through intact.
    wil::com_ptr<ID2D1Effect> multiply;
    RETURN_IF_FAILED(m_d2dContext->CreateEffect(CLSID_D2D1ArithmeticComposite, multiply.put()));
    multiply->SetInput(0, tile.bitmap.get());
    multiply->SetInputEffect(1, table.get());
    RETURN_IF_FAILED(multiply->SetValue(D2D1_ARITHMETICCOMPOSITE_PROP_COEFFICIENTS,
                                        D2D1::Vector4F(1.0f, 0.0f, 0.0f, 0.0f)));

    // Applied straight to the premultiplied values: the matrix is linear in
    // RGB and leaves alpha alone, so premultiplication commutes with it.
    wil::com_ptr<ID2D1Effect> matrix;
    RETURN_IF_FAILED(m_d2dContext->CreateEffect(CLSID_D2D1ColorMatrix, matrix.put()));
    matrix->SetInputEffect(0, multiply.get());
    RETURN_IF_FAILED(matrix->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE,
                                      D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED));

    // An effect's output otherwise follows its inputs' precision - 8-bit
    // UNORM here - which clamps the boosted values to 1.0 (measured: the
    // highlights stopped exactly at SDR white) and quantizes the linear
    // pixels.
    for (ID2D1Effect* effect : {stretch.get(), table.get(), multiply.get(), matrix.get()})
    {
        RETURN_IF_FAILED(
            effect->SetValue(D2D1_PROPERTY_PRECISION, D2D1_BUFFER_PRECISION_16BPC_FLOAT));
    }

    tile.gainTable = std::move(table);
    tile.colorMatrix = std::move(matrix);
    return S_OK;
}

void ImageRenderer::UpdateGainEffects()
{
    if (!m_gainBitmap)
    {
        return;
    }
    std::array<float, kGainTableSize> table{};
    const float divisor = BuildAppleGainTable(
        m_contentHeadroom, GainMapWeight(m_displayHeadroom, m_contentHeadroom), table);

    // D2D1_MATRIX_5X4_F multiplies a row vector (out = [r g b a 1] * M), so
    // m_toSrgb, written for column vectors, goes in transposed.
    const ColorMatrix3& m = m_toSrgb;
    const D2D1_MATRIX_5X4_F color = D2D1::Matrix5x4F(
        divisor * m[0], divisor * m[3], divisor * m[6], 0.0f,  //
        divisor * m[1], divisor * m[4], divisor * m[7], 0.0f,  //
        divisor * m[2], divisor * m[5], divisor * m[8], 0.0f,  //
        0.0f, 0.0f, 0.0f, 1.0f,                                //
        0.0f, 0.0f, 0.0f, 0.0f);

    const auto* bytes = reinterpret_cast<const BYTE*>(table.data());
    const auto size = static_cast<UINT32>(sizeof(table));
    for (auto& tile : m_tiles)
    {
        if (!tile.gainTable)
        {
            continue;
        }
        (void)tile.gainTable->SetValue(D2D1_TABLETRANSFER_PROP_RED_TABLE, bytes, size);
        (void)tile.gainTable->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_TABLE, bytes, size);
        (void)tile.gainTable->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_TABLE, bytes, size);
        (void)tile.colorMatrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, color);
    }
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
        if (tile.colorMatrix)
        {
            // The effect output is in the tile bitmap's pixel coordinates;
            // map them onto the view the same way DrawBitmap does below.
            m_d2dContext->SetTransform(
                D2D1::Matrix3x2F::Scale(layout.scale, layout.scale)
                * D2D1::Matrix3x2F::Translation(layout.destX + tile.withGutter.left * layout.scale,
                                                layout.destY + tile.withGutter.top * layout.scale));
            m_d2dContext->DrawImage(tile.colorMatrix.get(), D2D1::Point2F(src.left, src.top), src,
                                    D2D1_INTERPOLATION_MODE_LINEAR);
            m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        else
        {
            m_d2dContext->DrawBitmap(tile.bitmap.get(), dest, 1.0f,
                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
        }
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
