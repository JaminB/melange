cmake_minimum_required(VERSION 3.25)
project(WUMFix VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "WUMFix must be built for 32-bit x86 (WormsMayhem.exe is a 32-bit process). Use build.ps1.")
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

# Dear ImGui: no upstream CMakeLists, so populate the source only (SOURCE_SUBDIR points at a
# directory that doesn't exist in the checkout, which stops FetchContent from trying to
# add_subdirectory() it) and build a small static-library target ourselves. Pinned to a release
# tag, not the docking branch (see docs/m0-design.md U1). Linked but unused until component A.
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

# miniz: has its own CMakeLists.txt, so FetchContent_MakeAvailable builds it directly (a static
# "miniz" target; BUILD_EXAMPLES/BUILD_TESTS/INSTALL_PROJECT default off because PROJECT_NAME is
# already defined when its CMakeLists runs). Pinned to a release tag. Linked but unused until D.
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.1.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(miniz)

# Every .cpp under src/ is compiled into the plugin. Adding a module = dropping a file under src/.
file(GLOB_RECURSE WUMFIX_SOURCES CONFIGURE_DEPENDS src/*.cpp)
configure_file(src/core/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

add_library(WUMFix SHARED ${WUMFIX_SOURCES} src/core/version.rc)
set_target_properties(WUMFix PROPERTIES SUFFIX ".asi" PREFIX "")
# src/sdk is on the include path in its own right so frozen public headers include as "wumfix/<name>.h"
# (src/sdk/wumfix/<name>.h) rather than "sdk/wumfix/<name>.h".
target_include_directories(WUMFix PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(WUMFix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(WUMFix PRIVATE /W4 /utf-8 /permissive- /Zi /wd4100)
target_link_options(WUMFix PRIVATE /DEBUG /OPT:REF /OPT:ICF /SAFESEH:NO)
target_link_libraries(WUMFix PRIVATE safetyhook::safetyhook dbghelp bcrypt ws2_32 shell32 imgui miniz)

# Offline self-test for the event bus (component B): runs the real registry/dispatch/decoder code against a
# fake name table and fake messages, no game needed. Not part of the default build; scripts\selftest.ps1
# builds and runs it.
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

# Offline self-test for component C (structured JSONL logging): exercises src/core/jlog.cpp directly - session
# folders, rotation, the tail ring, JSON schema/validity and crash-flush - without the game or the .asi. See
# docs/m0-design.md SS3 "C" acceptance items 1, 6 and 7, and the M0 report for component C.
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
