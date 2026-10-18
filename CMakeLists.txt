cmake_minimum_required(VERSION 3.21)
project(CrabeReShade LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "CrabeReShade must be built for Win32 (-A Win32): the game is 32-bit.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(CrabeReShade SHARED
    src/plugin.cpp
    src/reshade_host.cpp
)
set_target_properties(CrabeReShade PROPERTIES SUFFIX ".asi")
target_include_directories(CrabeReShade PRIVATE sdk src)
target_compile_definitions(CrabeReShade PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(CrabeReShade PRIVATE /W4 /WX /utf-8)
target_link_libraries(CrabeReShade PRIVATE d3d11 dxgi)

option(CRABE_RESHADE_BUILD_TESTS "Build the companion smoke test (needs RESHADE_SOURCE)." OFF)
if(CRABE_RESHADE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
