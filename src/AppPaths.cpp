#include "AppPaths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <wil/stl.h>
#include <wil/win32_helpers.h>

#include <string>

namespace AppPaths
{

std::filesystem::path ExePath() noexcept
try
{
    // Grows the buffer until the whole path fits. GetModuleFileNameW on its
    // own truncates without saying so.
    std::wstring path;
    if (FAILED(wil::GetModuleFileNameW<std::wstring>(nullptr, path)))
    {
        return {};
    }
    return std::filesystem::path(std::move(path));
}
catch (...)
{
    return {};
}

std::filesystem::path ExeDirectory() noexcept
try
{
    return ExePath().parent_path();
}
catch (...)
{
    return {};
}

}  // namespace AppPaths
