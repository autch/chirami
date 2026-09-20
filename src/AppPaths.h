#pragma once

#include <filesystem>

// Locations derived from the running executable. Nothing here assumes a path
// length: the raw APIs truncate silently rather than fail. See DESIGN.md,
// "String comparison and path identity".
namespace AppPaths
{

// Full path of the running chirami.exe. Empty when the OS call fails;
// callers treat that as "location unknown" rather than guessing.
std::filesystem::path ExePath() noexcept;

// Directory holding the running chirami.exe. Empty when ExePath() failed.
std::filesystem::path ExeDirectory() noexcept;

}  // namespace AppPaths
