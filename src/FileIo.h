#pragma once

#include "framework.h"

#include <algorithm>
#include <vector>

// Reads `out.size()` bytes from `file` in 1 MiB chunks. `shouldAbort()` is
// called before each chunk so a load on a slow SMB share or OneDrive
// hydration can be abandoned when a newer request supersedes it.
//
// Known limit: a single ReadFile against a fully unresponsive share can
// still block until the redirector times out (CancelSynchronousIo later).
template <typename ShouldAbort>
HRESULT ReadFileChunked(HANDLE file, std::vector<uint8_t>& out, ShouldAbort&& shouldAbort) noexcept
try
{
    constexpr DWORD kChunkBytes = 1u << 20;  // 1 MiB
    size_t offset = 0;
    while (offset < out.size())
    {
        if (shouldAbort())
        {
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        const DWORD toRead =
            static_cast<DWORD>(std::min<size_t>(kChunkBytes, out.size() - offset));
        DWORD read = 0;
        RETURN_IF_WIN32_BOOL_FALSE(ReadFile(file, out.data() + offset, toRead, &read, nullptr));
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF), read == 0);
        offset += read;
    }
    return S_OK;
}
CATCH_RETURN()
