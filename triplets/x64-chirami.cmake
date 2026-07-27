# Custom triplet: everything static (library and CRT) like
# x64-windows-static, except libjpeg-turbo which is built as a DLL so it can
# ship next to the exe as the optional, deletable JPEG codec. Its CRT stays
# static too, keeping the portable ZIP free of any VC++ redistributable
# dependency.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "libjpeg-turbo")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
