cmake_minimum_required(VERSION 3.24)
project(parallax_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: games do not reliably ship the VC++ redistributable we would need.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(openxr_sdk
    URL https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.65/openxr_loader_windows-1.1.65.zip
    URL_HASH SHA256=859d7de35b7a968112db770b09ed8bddeed3cb16fd8ae070643a857676bea6fd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4)
FetchContent_MakeAvailable(openxr_sdk minhook)

# The VR core, shared by every engine: the Unity plugin's parallax_xr.dll and the Godot extension.
add_library(parallax_core OBJECT
    src/log.cpp
    src/xr.cpp
    src/gpu.cpp
    src/d3d11.cpp
    src/d3d12.cpp
    src/input.cpp
    src/settings.cpp
    src/view.cpp)
add_library(parallax_core_deps INTERFACE)
target_include_directories(parallax_core_deps INTERFACE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(parallax_core_deps INTERFACE XR_NO_PROTOTYPES XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(parallax_core_deps INTERFACE minhook d3d11 d3d12 dxgi d3dcompiler)
target_link_libraries(parallax_core PRIVATE parallax_core_deps)
target_compile_options(parallax_core PRIVATE /W4 /permissive-)

add_library(parallax_xr SHARED $<TARGET_OBJECTS:parallax_core>)
target_link_libraries(parallax_xr PRIVATE parallax_core_deps)

add_custom_command(TARGET parallax_xr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${openxr_sdk_SOURCE_DIR}/x64/bin/openxr_loader.dll $<TARGET_FILE_DIR:parallax_xr>)

# Theater for games Parallax cannot go inside: captures the game's window and shows it through parallax_xr.dll.
add_executable(parallax_theater WIN32 src/screen.cpp)
set_target_properties(parallax_theater PROPERTIES OUTPUT_NAME ParallaxTheater)
target_include_directories(parallax_theater PRIVATE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(parallax_theater PRIVATE XR_NO_PROTOTYPES XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(parallax_theater PRIVATE /W4 /permissive- /EHsc)
target_link_options(parallax_theater PRIVATE /ENTRY:wmainCRTStartup)
target_link_libraries(parallax_theater PRIVATE parallax_xr d3d11 dxgi dwmapi windowsapp)

# Godot 4.3+: the same core as a GDExtension, which a Godot game loads at runtime (GDExtensionManager.load_extension).
option(PARALLAX_GODOT "Build the Godot extension" ON)
if(PARALLAX_GODOT)
    FetchContent_Declare(godot_cpp
        GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
        GIT_TAG godot-4.3-stable
        GIT_SHALLOW TRUE)
    # godot-cpp picks its build from CMAKE_BUILD_TYPE (Debug when empty) and hard-codes /MD or /MDd: built as release,
    # its runtime flag removed so it takes our static runtime like the rest (games need no VC++ redistributable).
    set(CMAKE_BUILD_TYPE Release)
    FetchContent_MakeAvailable(godot_cpp)
    get_target_property(godot_cpp_flags godot-cpp COMPILE_FLAGS)
    string(REGEX REPLACE "/MDd?" "" godot_cpp_flags "${godot_cpp_flags}")
    set_target_properties(godot-cpp PROPERTIES COMPILE_FLAGS "${godot_cpp_flags}")
    add_library(parallax_godot SHARED src/godot.cpp src/bridge.cpp $<TARGET_OBJECTS:parallax_core>)
    target_link_libraries(parallax_godot PRIVATE parallax_core_deps godot-cpp)
    target_compile_options(parallax_godot PRIVATE /EHsc)
    add_custom_command(TARGET parallax_godot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${openxr_sdk_SOURCE_DIR}/x64/bin/openxr_loader.dll $<TARGET_FILE_DIR:parallax_godot>)
endif()

enable_testing()
add_executable(gesture_test test/gesture_test.cpp)
add_test(NAME gesture COMMAND gesture_test)
