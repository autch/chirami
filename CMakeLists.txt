cmake_minimum_required(VERSION 3.30)

project(chirami VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT so the portable ZIP needs no VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(wil CONFIG REQUIRED)

# WTL is installed as plain headers by vcpkg (no CMake config)
find_path(WTL_INCLUDE_DIR atlapp.h REQUIRED)

add_executable(chirami WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/ImageRenderer.cpp
    src/ImageLoader.cpp
    src/ImageSaver.cpp
    src/MetadataReader.cpp
    src/MetadataWindow.cpp
    src/TurboJpeg.cpp
    src/ImageTransform.cpp
    src/FolderScanner.cpp
    src/FileAssociation.cpp
    src/WicDecoders.cpp
    src/Settings.cpp
    src/chirami.rc
    src/chirami.manifest
)

target_include_directories(chirami PRIVATE "${WTL_INCLUDE_DIR}")

target_compile_definitions(chirami PRIVATE
    UNICODE
    _UNICODE
    "CHIRAMI_VERSION=L\"${PROJECT_VERSION}\""
)

target_link_libraries(chirami PRIVATE
    WIL::WIL
    d2d1
    d3d11
    dxguid
    dwrite
    windowscodecs
    shlwapi
    shell32
    comctl32
)

target_compile_options(chirami PRIVATE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
)

# The optional runtime JPEG codec (see DESIGN.md). The x64-chirami overlay
# triplet builds libjpeg-turbo as a DLL with a static CRT; keep it next to
# the exe, which is where TurboJpeg.cpp loads it from (full path, no search).
add_custom_command(TARGET chirami POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/bin/turbojpeg.dll"
        "$<TARGET_FILE_DIR:chirami>"
)
