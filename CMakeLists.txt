cmake_minimum_required(VERSION 3.25)
project(Melange VERSION 0.2.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Melange must be built for 32-bit x86 (WormsMayhem.exe is a 32-bit process). Use build.ps1.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the .asi has no runtime DLL dependencies on players' machines.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
set(SAFETYHOOK_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG v0.7.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(safetyhook)

# Dear ImGui has no CMakeLists; the bogus SOURCE_SUBDIR stops FetchContent from add_subdirectory()-ing it.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR not-a-cmake-project)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_link_libraries(imgui PUBLIC opengl32)

FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.1.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(miniz)

# Header-only stb_image for PNG loading.
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz)
FetchContent_MakeAvailable(stb)

# Lua 5.4.7 for the Sandbox VM, compiled as C++ so that lua_error throws and destructors in bindings run.
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR not-a-cmake-project)
FetchContent_MakeAvailable(lua)
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua54 STATIC ${LUA_SOURCES})
# Upstream lua.hpp wraps the headers in extern "C", which does not match a C++ build of Lua.
file(WRITE ${CMAKE_BINARY_DIR}/lua-include/lua.hpp
    "#pragma once\n#include \"lua.h\"\n#include \"lualib.h\"\n#include \"lauxlib.h\"\n")
target_include_directories(lua54 PUBLIC ${lua_SOURCE_DIR} ${CMAKE_BINARY_DIR}/lua-include)
target_compile_options(lua54 PRIVATE /TP /EHsc /W1 /wd4244 /wd4267 /wd4334)

# Every .cpp under src/ is compiled into the plugin.
file(GLOB_RECURSE MELANGE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER MELANGE_SOURCES EXCLUDE REGEX "/src/xom/")

# Optional out-of-tree modules (<dir>/modules/*.cpp), e.g. build.ps1 -PrivateDir <dir>.
set(MELANGE_PRIVATE_DIR "" CACHE PATH "Optional folder with extra modules (modules/*.cpp)")
if(MELANGE_PRIVATE_DIR)
    file(GLOB_RECURSE MELANGE_PRIVATE_SOURCES CONFIGURE_DEPENDS ${MELANGE_PRIVATE_DIR}/modules/*.cpp)
    list(FILTER MELANGE_PRIVATE_SOURCES EXCLUDE REGEX "/overlay_tests/")
    list(APPEND MELANGE_SOURCES ${MELANGE_PRIVATE_SOURCES})
endif()
add_subdirectory(src/xom EXCLUDE_FROM_ALL)
configure_file(src/core/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

add_library(melange SHARED ${MELANGE_SOURCES} src/core/version.rc)
set_target_properties(melange PROPERTIES SUFFIX ".asi" PREFIX "")
# src/sdk is on the include path so public headers include as "melange/<name>.h".
target_include_directories(melange PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated ${stb_SOURCE_DIR})
target_compile_definitions(melange PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(melange PRIVATE /W4 /utf-8 /permissive- /Zi /wd4100)
target_link_options(melange PRIVATE /DEBUG /OPT:REF /OPT:ICF /SAFESEH:NO)
target_link_libraries(melange PRIVATE safetyhook::safetyhook dbghelp bcrypt ws2_32 shell32 imgui miniz lua54)

add_executable(json_read_selftest tests/json_read_selftest.cpp src/tools/json_read.cpp)
target_include_directories(json_read_selftest PRIVATE src)
target_compile_definitions(json_read_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(json_read_selftest PRIVATE /W4 /utf-8 /permissive-)

add_executable(lua54_selftest tests/lua54_selftest.cpp)
target_compile_options(lua54_selftest PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(lua54_selftest PRIVATE lua54)

add_executable(thumper_selftest
    tests/thumper_selftest.cpp
    src/mods/spice.cpp
    src/tools/json_read.cpp
    src/core/log.cpp)
target_include_directories(thumper_selftest PRIVATE src src/sdk)
target_compile_definitions(thumper_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(thumper_selftest PRIVATE /W4 /utf-8 /permissive-)

# Offline self-tests (no game needed).
add_executable(bus_selftest EXCLUDE_FROM_ALL
    tests/bus_selftest.cpp
    src/core/bus.cpp src/core/bus_registry.cpp src/core/bus_decoders.cpp
    src/core/events.cpp src/core/game.cpp src/core/mem.cpp src/core/log.cpp src/core/config.cpp
    src/core/testcmd.cpp src/core/module.cpp)
target_include_directories(bus_selftest PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(bus_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(bus_selftest PRIVATE /W4 /utf-8 /permissive- /Zi /wd4100)
target_link_options(bus_selftest PRIVATE /DEBUG /SAFESEH:NO)
target_link_libraries(bus_selftest PRIVATE safetyhook::safetyhook bcrypt)

add_executable(trace_selftest EXCLUDE_FROM_ALL
    tests/trace_selftest.cpp
    src/render/mirage/trace_decode.cpp src/render/mirage/trace_mcap.cpp src/render/mirage/compat_report.cpp
    src/core/events.cpp src/core/game.cpp src/core/mem.cpp src/core/log.cpp)
target_include_directories(trace_selftest PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated ${stb_SOURCE_DIR})
target_compile_definitions(trace_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(trace_selftest PRIVATE /W4 /utf-8 /permissive- /Zi /wd4100)
target_link_options(trace_selftest PRIVATE /DEBUG /SAFESEH:NO)
target_link_libraries(trace_selftest PRIVATE safetyhook::safetyhook bcrypt miniz)

add_executable(gldebug_selftest EXCLUDE_FROM_ALL tests/gldebug_selftest.cpp)
target_include_directories(gldebug_selftest PRIVATE src)
target_compile_options(gldebug_selftest PRIVATE /W4 /utf-8 /permissive-)

add_executable(postfx_selftest EXCLUDE_FROM_ALL
    tests/postfx_selftest.cpp
    src/render/mirage/postfx_effect.cpp
    src/render/mirage/postfx_gl.cpp)
target_include_directories(postfx_selftest PRIVATE src src/sdk ${stb_SOURCE_DIR})
target_compile_definitions(postfx_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELANGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(postfx_selftest PRIVATE /W4 /utf-8 /permissive- /wd4100)
target_link_libraries(postfx_selftest PRIVATE opengl32 gdi32 user32)

add_executable(draw_queue_selftest EXCLUDE_FROM_ALL tests/draw_queue_selftest.cpp)
target_include_directories(draw_queue_selftest PRIVATE src src/sdk)
target_compile_options(draw_queue_selftest PRIVATE /W4 /utf-8 /permissive-)

add_executable(jlog_selftest
    tests/jlog_selftest.cpp
    src/core/jlog.cpp
    src/core/jlog_bus_filter.cpp
    src/core/events.cpp
    src/core/game.cpp
    src/core/mem.cpp
    src/core/log.cpp)
target_include_directories(jlog_selftest PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(jlog_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(jlog_selftest PRIVATE /W4 /utf-8 /permissive- /wd4100)
target_link_libraries(jlog_selftest PRIVATE bcrypt psapi shell32 ole32)

add_executable(shaders_selftest
    tests/shaders_selftest.cpp
    src/render/mirage/shaders_source.cpp
    src/render/mirage/shaders_fixes.cpp)
target_include_directories(shaders_selftest PRIVATE src)
target_compile_definitions(shaders_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELANGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(shaders_selftest PRIVATE /W4 /utf-8 /permissive- /wd4100)
