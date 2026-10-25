cmake_minimum_required(VERSION 3.24)
project(parallax_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: games do not reliably ship the VC++ redistributable we would need.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(openxr_sdk
    URL https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.65/openxr_loader_windows-1.1.65.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4)
FetchContent_MakeAvailable(openxr_sdk minhook)

add_library(parallax_xr SHARED
    src/log.cpp
    src/xr.cpp
    src/gpu.cpp
    src/d3d11.cpp
    src/d3d12.cpp
    src/input.cpp)
target_include_directories(parallax_xr PRIVATE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(parallax_xr PRIVATE XR_NO_PROTOTYPES XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(parallax_xr PRIVATE /W4 /permissive-)
target_link_libraries(parallax_xr PRIVATE minhook d3d11 d3d12 dxgi d3dcompiler)

add_custom_command(TARGET parallax_xr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${openxr_sdk_SOURCE_DIR}/x64/bin/openxr_loader.dll $<TARGET_FILE_DIR:parallax_xr>)

enable_testing()
add_executable(gesture_test test/gesture_test.cpp)
add_test(NAME gesture COMMAND gesture_test)
