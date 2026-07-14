#include "TurboJpeg.h"

#include <turbojpeg.h>

#include <filesystem>
#include <mutex>

namespace TurboJpeg
{
namespace
{

// tj3Init is a macro over tj3InitVersion in TurboJPEG >= 3.2, so both entry
// points get plain typedefs; older 3.x DLLs only export tj3Init.
using Tj3InitVersionFn = tjhandle(DLLCALL*)(int initType, int apiVersion);
using Tj3InitFn = tjhandle(DLLCALL*)(int initType);

struct Api
{
    wil::unique_hmodule module;
    Tj3InitVersionFn initVersion = nullptr;
    Tj3InitFn initPlain = nullptr;
    decltype(&tj3Destroy) destroy = nullptr;
    decltype(&tj3DecompressHeader) decompressHeader = nullptr;
    decltype(&tj3Get) get = nullptr;
    decltype(&tj3Decompress8) decompress8 = nullptr;

    bool Loaded() const { return module.get() != nullptr; }

    tjhandle CreateDecompressor() const
    {
        return initVersion != nullptr ? initVersion(TJINIT_DECOMPRESS, TURBOJPEG_VERSION_NUMBER)
                                      : initPlain(TJINIT_DECOMPRESS);
    }
};

const Api& GetApi()
{
    static Api api;
    static std::once_flag once;
    std::call_once(once, [] {
        // Only the DLL sitting next to the exe is trusted; never search the
        // path. Deleting the DLL cleanly disables the codec (WIC fallback).
        WCHAR buffer[MAX_PATH];
        if (GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer)) == 0)
        {
            return;
        }
        const std::filesystem::path dllPath =
            std::filesystem::path(buffer).parent_path() / L"turbojpeg.dll";
        wil::unique_hmodule module(LoadLibraryExW(dllPath.c_str(), nullptr, 0));
        if (!module)
        {
            return;
        }

        const auto resolve = [&](auto& pointer, const char* name) {
            pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(
                GetProcAddress(module.get(), name));
            return pointer != nullptr;
        };
        Api loaded;
        const bool initOk =
            resolve(loaded.initVersion, "tj3InitVersion") | resolve(loaded.initPlain, "tj3Init");
        if (initOk && resolve(loaded.destroy, "tj3Destroy")
            && resolve(loaded.decompressHeader, "tj3DecompressHeader")
            && resolve(loaded.get, "tj3Get") && resolve(loaded.decompress8, "tj3Decompress8"))
        {
            loaded.module = std::move(module);
            api = std::move(loaded);
        }
    });
    return api;
}

}  // namespace

bool IsAvailable()
{
    return GetApi().Loaded();
}

bool LooksLikeJpeg(const uint8_t* data, size_t size)
{
    return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

HRESULT Decode(const uint8_t* data, size_t size, LoadedImage& out) noexcept
try
{
    const Api& api = GetApi();
    RETURN_HR_IF(E_NOTIMPL, !api.Loaded());

    const tjhandle handle = api.CreateDecompressor();
    RETURN_HR_IF(E_FAIL, handle == nullptr);
    auto destroy = wil::scope_exit([&] { api.destroy(handle); });

    RETURN_HR_IF(E_FAIL, api.decompressHeader(handle, data, size) != 0);
    const int width = api.get(handle, TJPARAM_JPEGWIDTH);
    const int height = api.get(handle, TJPARAM_JPEGHEIGHT);
    RETURN_HR_IF(WINCODEC_ERR_BADIMAGE, width <= 0 || height <= 0);

    const uint64_t stride64 = uint64_t{static_cast<uint32_t>(width)} * 4;
    const uint64_t total64 = stride64 * static_cast<uint32_t>(height);
    RETURN_HR_IF(kHrImageTooLarge, total64 > kMaxPixelBytes);

    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.stride = static_cast<uint32_t>(stride64);
    out.pixels.resize(static_cast<size_t>(total64));

    // TJPF_BGRA writes opaque alpha, which doubles as premultiplied BGRA.
    // Anything TurboJPEG rejects (e.g. CMYK) falls back to WIC upstream.
    if (api.decompress8(handle, data, size, out.pixels.data(), width * 4, TJPF_BGRA) != 0)
    {
        out = {};
        return E_FAIL;
    }
    return S_OK;
}
CATCH_RETURN()

}  // namespace TurboJpeg
