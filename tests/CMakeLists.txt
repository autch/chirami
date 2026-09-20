# Unit tests for the pure logic. Nothing here may pull in framework.h: the
# test executable has no CAppModule, so WTL-dependent headers would not link
# (see DESIGN.md, "Test policy").
find_package(Catch2 3 CONFIG REQUIRED)

add_executable(chirami_tests
    PathCompareTests.cpp
    ViewLayoutTests.cpp
    "${CMAKE_SOURCE_DIR}/src/PathCompare.cpp"
)

target_include_directories(chirami_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")

target_compile_definitions(chirami_tests PRIVATE
    UNICODE
    _UNICODE
    # framework.h sets these before <windows.h> for the app; a header under
    # test reaches <windows.h> on its own, so the tests set them as well -
    # without NOMINMAX the min/max macros break std::min in ViewLayout.h.
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(chirami_tests PRIVATE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
)

target_link_libraries(chirami_tests PRIVATE Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(chirami_tests)
