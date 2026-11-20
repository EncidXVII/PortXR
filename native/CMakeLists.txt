# Copyright (C) 2026 EncidXVII
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.24)
project(portxr_native LANGUAGES C CXX)

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
# stb_truetype and stb_image (public domain / MIT) for the VR menu's text and cover art; pinned to a reviewed commit.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 9600522de53973336fa033a289858a4354cbb04c)
FetchContent_MakeAvailable(openxr_sdk minhook stb)

# Lua 5.4.7 (MIT, lua.org) for per-game scripts, built as C++ so its errors unwind our calls' destructors.
file(GLOB lua_sources ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/*.c)
set_source_files_properties(${lua_sources} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${lua_sources})
target_include_directories(lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua)
target_compile_options(lua PRIVATE /EHsc /W0)

# The app's fonts, embedded in the core for the VR menu.
include(${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake)
set(assets ${CMAKE_CURRENT_SOURCE_DIR}/../app/assets)
embed_files(${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp
    kBarlowRegular ${assets}/Barlow-Regular.ttf
    kBarlowSemiBold ${assets}/Barlow-SemiBold.ttf
    kAnton ${assets}/Anton-Regular.ttf)

# The VR core, shared by every engine: the Unity plugin's portxr_xr.dll and the Godot extension.
add_library(portxr_core OBJECT
    src/log.cpp
    src/xr.cpp
    src/gpu.cpp
    src/d3d11.cpp
    src/d3d12.cpp
    src/input.cpp
    src/settings.cpp
    src/rig.cpp
    src/canvas.cpp
    src/menu.cpp
    src/bridge.cpp
    src/script.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp)
add_library(portxr_core_deps INTERFACE)
target_include_directories(portxr_core_deps INTERFACE ${openxr_sdk_SOURCE_DIR}/include ${stb_SOURCE_DIR})
target_compile_definitions(portxr_core_deps INTERFACE XR_NO_PROTOTYPES XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(portxr_core_deps INTERFACE minhook lua d3d11 d3d12 dxgi d3dcompiler)
target_link_libraries(portxr_core PRIVATE portxr_core_deps)
target_compile_options(portxr_core PRIVATE /W4 /permissive-)

add_library(portxr_xr SHARED $<TARGET_OBJECTS:portxr_core>)
target_link_libraries(portxr_xr PRIVATE portxr_core_deps)

add_custom_command(TARGET portxr_xr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${openxr_sdk_SOURCE_DIR}/x64/bin/openxr_loader.dll $<TARGET_FILE_DIR:portxr_xr>)

# Unreal Engine games: the core inside the game, loaded as dwmapi.dll from beside its Shipping exe.
add_library(portxr_unreal SHARED src/unreal.cpp src/uobject.cpp $<TARGET_OBJECTS:portxr_core>)
set_target_properties(portxr_unreal PROPERTIES OUTPUT_NAME dwmapi)
target_link_libraries(portxr_unreal PRIVATE portxr_core_deps)
target_compile_options(portxr_unreal PRIVATE /W4 /permissive- /EHsc)
add_custom_command(TARGET portxr_unreal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${openxr_sdk_SOURCE_DIR}/x64/bin/openxr_loader.dll $<TARGET_FILE_DIR:portxr_unreal>)

# Theater for games PortXR cannot go inside: captures the game's window and shows it through portxr_xr.dll.
add_executable(portxr_theater WIN32 src/screen.cpp)
set_target_properties(portxr_theater PROPERTIES OUTPUT_NAME PortXRTheater)
target_include_directories(portxr_theater PRIVATE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(portxr_theater PRIVATE XR_NO_PROTOTYPES XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(portxr_theater PRIVATE /W4 /permissive- /EHsc)
target_link_options(portxr_theater PRIVATE /ENTRY:wmainCRTStartup)
target_link_libraries(portxr_theater PRIVATE portxr_xr d3d11 dxgi dwmapi windowsapp)

# Godot 4.3+: the same core as a GDExtension, which a Godot game loads at runtime (GDExtensionManager.load_extension).
option(PORTXR_GODOT "Build the Godot extension" ON)
if(PORTXR_GODOT)
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
    add_library(portxr_godot SHARED src/godot.cpp $<TARGET_OBJECTS:portxr_core>)
    target_link_libraries(portxr_godot PRIVATE portxr_core_deps godot-cpp)
    target_compile_options(portxr_godot PRIVATE /EHsc)
    add_custom_command(TARGET portxr_godot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${openxr_sdk_SOURCE_DIR}/x64/bin/openxr_loader.dll $<TARGET_FILE_DIR:portxr_godot>)
endif()

enable_testing()
add_executable(gesture_test test/gesture_test.cpp)
add_test(NAME gesture COMMAND gesture_test)
