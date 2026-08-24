cmake_minimum_required(VERSION 3.21)
project(acodyssey_dlaa VERSION 1.0.0 LANGUAGES C CXX)

set(MINHOOK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook"
    CACHE PATH "MinHook source root"
)

set(NGX_SDK_ROOT "" CACHE PATH "NVIDIA NGX SDK root")
set(NGX_RUNTIME "" CACHE FILEPATH "NVIDIA DLSS production runtime")

if(NOT NGX_SDK_ROOT AND DEFINED ENV{NGX_SDK_ROOT})
    set(NGX_SDK_ROOT "$ENV{NGX_SDK_ROOT}" CACHE PATH "NVIDIA NGX SDK root" FORCE)
endif()
if(NOT NGX_RUNTIME AND DEFINED ENV{NGX_RUNTIME})
    set(NGX_RUNTIME "$ENV{NGX_RUNTIME}" CACHE FILEPATH "NVIDIA DLSS production runtime" FORCE)
endif()

if(NOT EXISTS "${MINHOOK_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "MinHook was not found at ${MINHOOK_ROOT}")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static third-party libraries" FORCE)
add_subdirectory("${MINHOOK_ROOT}" "${CMAKE_BINARY_DIR}/third_party/minhook" EXCLUDE_FROM_ALL)

add_library(acodyssey-dinput8-proxy SHARED
    src/proxy.cpp
    src/bridge_api.h
    src/proxy.def
)

set(FXC_EXECUTABLE "" CACHE FILEPATH "Path to fxc.exe")
if(NOT FXC_EXECUTABLE)
    find_program(FXC_EXECUTABLE NAMES fxc.exe)
endif()
if(NOT FXC_EXECUTABLE)
    file(GLOB FXC_CANDIDATES
        "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe"
        "C:/Program Files (x86)/Windows Kits/10/bin/*/x86/fxc.exe"
    )
    if(FXC_CANDIDATES)
        list(SORT FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET FXC_CANDIDATES 0 FXC_EXECUTABLE)
    endif()
endif()
if(NOT FXC_EXECUTABLE)
    message(FATAL_ERROR "fxc.exe was not found; set FXC_EXECUTABLE")
endif()
if(NOT EXISTS "${NGX_SDK_ROOT}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NGX SDK was not found; set NGX_SDK_ROOT")
endif()
if(NOT EXISTS "${NGX_RUNTIME}")
    message(FATAL_ERROR "NGX production runtime was not found; set NGX_RUNTIME")
endif()

set(MOTION_SHADER "${CMAKE_BINARY_DIR}/bin/ACOdysseyDLAA.motion_decode.cso")
add_custom_command(
    OUTPUT "${MOTION_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin"
    COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_0 /E main /O3 /Ges
            /Fo "${MOTION_SHADER}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_decode.hlsl"
    DEPENDS shaders/motion_decode.hlsl
    VERBATIM
)

set(SHARPEN_SHADER "${CMAKE_BINARY_DIR}/bin/ACOdysseyDLAA.sharpen.cso")
add_custom_command(
    OUTPUT "${SHARPEN_SHADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin"
    COMMAND "${FXC_EXECUTABLE}" /nologo /T cs_5_0 /E main /O3 /Ges
            /Fo "${SHARPEN_SHADER}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sharpen.hlsl"
    DEPENDS shaders/sharpen.hlsl
    VERBATIM
)

add_library(acodyssey-dlss-bridge SHARED
    src/bridge.cpp
    src/bridge_api.h
    "${MOTION_SHADER}"
    "${SHARPEN_SHADER}"
)
target_compile_features(acodyssey-dlss-bridge PRIVATE cxx_std_20)
target_compile_definitions(acodyssey-dlss-bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(acodyssey-dlss-bridge PRIVATE "${NGX_SDK_ROOT}/include")
target_link_libraries(acodyssey-dlss-bridge PRIVATE
    d3d11
    version
    "${NGX_SDK_ROOT}/lib/Windows_x86_64/nvsdk_ngx_d.lib"
)

add_executable(d3d11-vtable-indices tests/vtable_indices.cpp)
target_compile_features(d3d11-vtable-indices PRIVATE cxx_std_17)

add_executable(execute-detour-smoke tests/execute_detour_smoke.cpp)
target_compile_features(execute-detour-smoke PRIVATE cxx_std_17)
target_include_directories(execute-detour-smoke PRIVATE "${MINHOOK_ROOT}/include")
target_link_libraries(execute-detour-smoke PRIVATE d3d11 minhook)

add_executable(bridge-smoke tests/bridge_smoke.cpp)
target_compile_features(bridge-smoke PRIVATE cxx_std_17)
target_compile_definitions(bridge-smoke PRIVATE UNICODE _UNICODE)
target_link_libraries(bridge-smoke PRIVATE d3d11 dxgi)

add_executable(ngx-synthetic-smoke tests/ngx_synthetic_smoke.cpp)
target_compile_features(ngx-synthetic-smoke PRIVATE cxx_std_17)
target_compile_definitions(ngx-synthetic-smoke PRIVATE UNICODE _UNICODE)
target_include_directories(ngx-synthetic-smoke PRIVATE src)
target_link_libraries(ngx-synthetic-smoke PRIVATE d3d11)

target_compile_features(acodyssey-dinput8-proxy PRIVATE cxx_std_20)
target_compile_definitions(acodyssey-dinput8-proxy PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    DIRECTINPUT_VERSION=0x0800
    UNICODE
    _UNICODE
)
target_link_libraries(acodyssey-dinput8-proxy PRIVATE
    minhook
    bcrypt
)

if(MSVC)
    target_compile_options(acodyssey-dinput8-proxy PRIVATE
        /W4
        /permissive-
        /guard:cf
        /utf-8
        /EHsc
    )
    target_link_options(acodyssey-dinput8-proxy PRIVATE /guard:cf)
    target_compile_options(d3d11-vtable-indices PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(execute-detour-smoke PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(bridge-smoke PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(ngx-synthetic-smoke PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(acodyssey-dlss-bridge PRIVATE /W4 /permissive- /guard:cf /utf-8 /EHsc)
    target_link_options(acodyssey-dlss-bridge PRIVATE /guard:cf)
endif()

set_target_properties(acodyssey-dinput8-proxy PROPERTIES
    OUTPUT_NAME dinput8
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(acodyssey-dlss-bridge PROPERTIES
    OUTPUT_NAME ACOdysseyDLSSBridge
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET acodyssey-dlss-bridge POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${NGX_RUNTIME}" "${CMAKE_BINARY_DIR}/bin/nvngx_dlss.dll"
)
