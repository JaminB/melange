# melange::xom - portable XOM reader/writer (C++17, no platform dependencies).
#
# Built two ways:
#   * from the root project: targets `melange_xom` (static lib) and `xom_roundtrip`
#     (test tool). Added with EXCLUDE_FROM_ALL, so the default build (melange.asi)
#     does not build or link it:  cmake --build --preset x86-release --target xom_roundtrip
#   * standalone, e.g. native 64-bit or WebAssembly:
#         cmake -S src/xom -B build/xom && cmake --build build/xom
#         emcmake cmake -S src/xom -B build/xom-wasm && cmake --build build/xom-wasm --target melange_xom
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_NAME)
    project(melange_xom LANGUAGES CXX)
endif()

add_library(melange_xom STATIC xom.cpp xom.h xom_schema.inc)
target_include_directories(melange_xom PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(melange_xom PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(melange_xom PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(melange_xom PRIVATE -Wall -Wextra)
endif()

if(NOT EMSCRIPTEN)
    add_executable(xom_roundtrip xom_roundtrip.cpp)
    target_link_libraries(xom_roundtrip PRIVATE melange_xom)
    set_target_properties(xom_roundtrip PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(xom_roundtrip PRIVATE /W4 /utf-8 /permissive-)
    endif()
endif()
