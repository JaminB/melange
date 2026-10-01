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

# civetweb (MIT) for the Oasis server: C, x86, no TLS, no CGI/Lua/Duktape, no file serving (NO_FILES). The patch makes the
# parser reject obs-fold and oversized header sets and the WebSocket reader reject unmasked or oversized frames.
FetchContent_Declare(civetweb
    URL https://github.com/civetweb/civetweb/archive/refs/tags/v1.16.tar.gz
    URL_HASH SHA256=f0e471c1bf4e7804a6cfb41ea9d13e7d623b2bcc7bc1e2a4dd54951a24d60285
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -DSRC=<SOURCE_DIR> -P ${CMAKE_SOURCE_DIR}/cmake/civetweb_patch.cmake
    SOURCE_SUBDIR not-a-cmake-project)
FetchContent_MakeAvailable(civetweb)
add_library(civetweb STATIC ${civetweb_SOURCE_DIR}/src/civetweb.c)
target_include_directories(civetweb PUBLIC ${civetweb_SOURCE_DIR}/include)
target_compile_definitions(civetweb PRIVATE NO_SSL USE_WEBSOCKET NO_CGI NO_FILES NO_THREAD_NAME NO_CACHING
    MG_MAX_UNANSWERED_PING=1 MG_OASIS_MAX_FRAME=1048576 _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
target_compile_options(civetweb PRIVATE /W0)
target_link_libraries(civetweb PUBLIC ws2_32)

# The Oasis server core (no game dependency). Users also link src/tools/json_read.cpp and src/core/log.cpp.
add_library(oasis_core STATIC
    src/oasis/core/server.cpp src/oasis/core/router.cpp src/oasis/core/http.cpp src/oasis/core/ws.cpp
    src/oasis/core/queue.cpp src/oasis/core/files_zip.cpp src/oasis/core/files_dir.cpp)
target_include_directories(oasis_core PUBLIC src src/sdk)
target_compile_definitions(oasis_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(oasis_core PRIVATE /W4 /utf-8 /permissive- /Zi)
target_link_libraries(oasis_core PUBLIC civetweb miniz ws2_32)

# The web bundle: scripts/web/build.ps1 (tsc + esbuild from tools\) into a deterministic zip, embedded as RCDATA.
# Without the toolchain (scripts\web\fetch.ps1) or with -DMELANGE_WEB=OFF, a placeholder page is embedded.
option(MELANGE_WEB "Build the Oasis web app into melange.asi" ON)
add_executable(oasis_pack src/oasis/pack/main.cpp)
target_compile_options(oasis_pack PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(oasis_pack PRIVATE miniz)
set(OASIS_WEB_ZIP ${CMAKE_BINARY_DIR}/oasis-web.zip)
set(OASIS_WEB_READY ${MELANGE_WEB})
foreach(f tools/node/node.exe tools/esbuild/esbuild.exe web/node_modules/typescript/package.json web/node_modules/preact/package.json)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/${f})
        set(OASIS_WEB_READY OFF)
    endif()
endforeach()
if(OASIS_WEB_READY)
    file(GLOB_RECURSE OASIS_WEB_SOURCES CONFIGURE_DEPENDS web/src/* web/public/*)
    add_custom_command(OUTPUT ${OASIS_WEB_ZIP}
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/scripts/web/build.ps1
                -Out ${CMAKE_BINARY_DIR}/web-dist
        COMMAND oasis_pack ${CMAKE_BINARY_DIR}/web-dist ${OASIS_WEB_ZIP}
        DEPENDS oasis_pack ${OASIS_WEB_SOURCES} ${CMAKE_SOURCE_DIR}/web/web.lock.json ${CMAKE_SOURCE_DIR}/web/tsconfig.json
                ${CMAKE_SOURCE_DIR}/scripts/web/build.ps1 ${CMAKE_SOURCE_DIR}/THIRD_PARTY.md
        COMMENT "Building the Oasis web app" VERBATIM)
else()
    message(STATUS "Oasis web app: toolchain not fetched or MELANGE_WEB=OFF, embedding the placeholder page")
    add_custom_command(OUTPUT ${OASIS_WEB_ZIP}
        COMMAND oasis_pack ${CMAKE_SOURCE_DIR}/web/placeholder ${OASIS_WEB_ZIP}
        DEPENDS oasis_pack ${CMAKE_SOURCE_DIR}/web/placeholder/index.html ${CMAKE_SOURCE_DIR}/web/placeholder/build.txt
        COMMENT "Packing the Oasis placeholder page" VERBATIM)
endif()
add_custom_target(oasis_web DEPENDS ${OASIS_WEB_ZIP})
set(OASIS_WEB_BUNDLED ${OASIS_WEB_READY} CACHE INTERNAL "The web app, not the placeholder, is embedded")
file(TO_NATIVE_PATH ${OASIS_WEB_ZIP} OASIS_WEB_ZIP_NATIVE)
string(REPLACE "\\" "\\\\" OASIS_WEB_ZIP_RC "${OASIS_WEB_ZIP_NATIVE}")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/oasis_web.rc CONTENT "OASIS_WEB RCDATA \"${OASIS_WEB_ZIP_RC}\"\n")
set_source_files_properties(${CMAKE_BINARY_DIR}/generated/oasis_web.rc PROPERTIES OBJECT_DEPENDS ${OASIS_WEB_ZIP})

# The Erg level service (scene load/build over src/xom, the project store, the registry bank), shared by the plugin's
# Oasis, oasis.exe, xomtool and the self-tests.
set(ERG_SERVICE_SOURCES
    src/erg/scene.cpp src/erg/patch.cpp src/erg/names.cpp src/erg/jsonio.cpp src/erg/xomutil.cpp src/erg/load.cpp
    src/erg/build.cpp src/erg/bank.cpp src/erg/install.cpp src/erg/project.cpp src/erg/service.cpp src/erg/pack.cpp src/erg/luagen.cpp src/erg/voxels.cpp src/erg/objects.cpp src/erg/hmp.cpp
    src/erg/preview.cpp src/levels/manifest.cpp src/mods/spice.cpp src/tools/json_read.cpp src/tools/hash.cpp
    src/core/log.cpp)

# Every .cpp under src/ is compiled into the plugin.
file(GLOB_RECURSE MELANGE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER MELANGE_SOURCES EXCLUDE REGEX "/src/xom/")
list(FILTER MELANGE_SOURCES EXCLUDE REGEX "/src/oasis/(core|pack|standalone)/")

# Optional out-of-tree modules (<dir>/modules/*.cpp), e.g. build.ps1 -PrivateDir <dir>.
set(MELANGE_PRIVATE_DIR "" CACHE PATH "Optional folder with extra modules (modules/*.cpp)")
if(MELANGE_PRIVATE_DIR)
    file(GLOB_RECURSE MELANGE_PRIVATE_SOURCES CONFIGURE_DEPENDS ${MELANGE_PRIVATE_DIR}/modules/*.cpp)
    list(FILTER MELANGE_PRIVATE_SOURCES EXCLUDE REGEX "/overlay_tests/")
    list(APPEND MELANGE_SOURCES ${MELANGE_PRIVATE_SOURCES})
endif()
add_subdirectory(src/xom EXCLUDE_FROM_ALL)
configure_file(src/core/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

add_library(melange SHARED ${MELANGE_SOURCES} src/core/version.rc ${CMAKE_BINARY_DIR}/generated/oasis_web.rc)
add_dependencies(melange oasis_web)
set_target_properties(melange PROPERTIES SUFFIX ".asi" PREFIX "")
# src/sdk is on the include path so public headers include as "melange/<name>.h".
target_include_directories(melange PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated ${stb_SOURCE_DIR})
target_compile_definitions(melange PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(melange PRIVATE /W4 /utf-8 /permissive- /Zi /wd4100)
target_link_options(melange PRIVATE /DEBUG /OPT:REF /OPT:ICF /SAFESEH:NO)
target_link_libraries(melange PRIVATE safetyhook::safetyhook dbghelp bcrypt ws2_32 shell32 winmm imgui miniz lua54 oasis_core
    melange_xom)

# Oasis core without the game: HTTP/WS corpus against a live server on 127.0.0.1, protocol and queue cases.
add_executable(oasis_core_selftest tests/oasis_core_selftest.cpp src/oasis/auth.cpp src/oasis/webpanels.cpp
    src/tools/json_read.cpp src/core/log.cpp)
target_include_directories(oasis_core_selftest PRIVATE src src/sdk)
target_compile_definitions(oasis_core_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(oasis_core_selftest PRIVATE /W4 /utf-8 /permissive- /Zi)
target_link_options(oasis_core_selftest PRIVATE /DEBUG)
target_link_libraries(oasis_core_selftest PRIVATE oasis_core bcrypt)

# A: the full Auth (token, per-port cookie, Host/Origin, the failure rate limiter), driven directly with
# synthetic Request/Response values -- no live server needed.
add_executable(oasis_auth_selftest tests/oasis_auth_selftest.cpp src/oasis/auth.cpp src/tools/json_read.cpp src/core/log.cpp)
target_include_directories(oasis_auth_selftest PRIVATE src src/sdk)
target_compile_definitions(oasis_auth_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(oasis_auth_selftest PRIVATE /W4 /utf-8 /permissive- /Zi)
target_link_libraries(oasis_auth_selftest PRIVATE oasis_core bcrypt)

# Game-state readers without the game: fake containers, descriptors, RTTI and task table; JSON and Oasis parameters.
add_executable(gamestate_selftest tests/gamestate_selftest.cpp src/game/state/gamestate_logic.cpp
    src/game/state/gamestate_json.cpp src/tools/json_read.cpp)
target_include_directories(gamestate_selftest PRIVATE src src/sdk)
target_compile_definitions(gamestate_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(gamestate_selftest PRIVATE /W4 /utf-8 /permissive-)

# Oasis panel RPCs without the game: the Melange.ini editor (ini.get/ini.set) and the lua.* parameter rules.
add_executable(oasis_rpc_selftest tests/oasis_rpc_selftest.cpp src/oasis/rpc/ini.cpp src/oasis/rpc/ini_edit.cpp
    src/core/config.cpp src/core/config_schema.cpp src/core/log.cpp src/tools/json_read.cpp)
target_include_directories(oasis_rpc_selftest PRIVATE src src/sdk)
target_compile_definitions(oasis_rpc_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(oasis_rpc_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(oasis_rpc_selftest PRIVATE oasis_core)

# oasis.exe: the standalone server, running with the game closed. Embeds the same web bundle as melange.asi.
add_executable(oasis_standalone
    src/oasis/standalone/main.cpp src/oasis/standalone/ini_edit.cpp src/oasis/standalone/mods_provider.cpp
    src/oasis/standalone/json_write.cpp src/oasis/standalone/game_lock.cpp src/oasis/standalone/wormsign_provider.cpp
    src/oasis/standalone/level_provider.cpp src/oasis/rpc/erg_assets.cpp src/assets/crcsafe_math.cpp src/assets/crcsafe_file.cpp
    src/oasis/auth.cpp src/oasis/webpanels.cpp src/oasis/rpc/ini_edit.cpp ${ERG_SERVICE_SOURCES}
    src/wormsign/format.cpp src/wormsign/records.cpp
    ${CMAKE_BINARY_DIR}/generated/oasis_web.rc)
add_dependencies(oasis_standalone oasis_web)
set_target_properties(oasis_standalone PROPERTIES OUTPUT_NAME "oasis")
target_include_directories(oasis_standalone PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated ${stb_SOURCE_DIR})
target_compile_definitions(oasis_standalone PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(oasis_standalone PRIVATE /W4 /utf-8 /permissive- /Zi)
target_link_options(oasis_standalone PRIVATE /DEBUG)
target_link_libraries(oasis_standalone PRIVATE oasis_core melange_xom bcrypt shell32 ole32)

# Offline self-test for the standalone-only file logic (ini editing, mods.list/setEnabled, wormsign.library): no
# game, no server.
add_executable(oasis_standalone_selftest
    tests/oasis_standalone_selftest.cpp
    src/oasis/standalone/ini_edit.cpp src/oasis/standalone/mods_provider.cpp src/oasis/standalone/json_write.cpp
    src/oasis/standalone/wormsign_provider.cpp src/wormsign/format.cpp src/wormsign/records.cpp
    src/mods/spice.cpp src/tools/json_read.cpp src/core/log.cpp)
target_include_directories(oasis_standalone_selftest PRIVATE src src/sdk)
target_compile_definitions(oasis_standalone_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(oasis_standalone_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(oasis_standalone_selftest PRIVATE miniz)

add_executable(json_read_selftest tests/json_read_selftest.cpp src/tools/json_read.cpp)
target_include_directories(json_read_selftest PRIVATE src)
target_compile_definitions(json_read_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(json_read_selftest PRIVATE /W4 /utf-8 /permissive-)

# The streams' pure wire logic (filters, bus name patterns, channel payloads) -- no game, no bus::, no Steam.
add_executable(oasis_streams_selftest
    tests/oasis_streams_selftest.cpp
    src/oasis/streams/wire.cpp
    src/tools/json_read.cpp)
target_include_directories(oasis_streams_selftest PRIVATE src src/sdk)
target_compile_definitions(oasis_streams_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(oasis_streams_selftest PRIVATE /W4 /utf-8 /permissive-)

add_executable(handshake_selftest
    tests/handshake_selftest.cpp
    src/mods/handshake_internal.cpp
    src/tools/hash.cpp)
target_include_directories(handshake_selftest PRIVATE src src/sdk)
target_compile_definitions(handshake_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(handshake_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(handshake_selftest PRIVATE bcrypt)

# The spice.json "weapons" array without the game: shape, names, bases, cells, set types and the k order.
add_executable(weapons_manifest_selftest
    tests/weapons_manifest_selftest.cpp
    src/weapons/manifest.cpp
    src/weapons/schema.cpp
    src/mods/spice.cpp
    src/tools/json_read.cpp
    src/core/log.cpp)
target_include_directories(weapons_manifest_selftest PRIVATE src src/sdk)
target_compile_definitions(weapons_manifest_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(weapons_manifest_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(weapons_manifest_selftest PRIVATE melange_xom)

# The Erg scene model without the game: stem rules, erg-scene/1 and erg-patch/1 over synthetic scenes, transforms and
# the spice.json "levels" array. --write regenerates tests/fixtures/erg.
add_executable(erg_scene_selftest
    tests/erg_scene_selftest.cpp
    src/erg/scene.cpp src/erg/patch.cpp src/erg/names.cpp src/erg/jsonio.cpp src/erg/luagen.cpp
    src/levels/manifest.cpp
    src/mods/spice.cpp
    src/tools/json_read.cpp
    src/core/log.cpp)
target_include_directories(erg_scene_selftest PRIVATE src src/sdk)
target_compile_definitions(erg_scene_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELANGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(erg_scene_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(erg_scene_selftest PRIVATE melange_xom)

# Asset previews (mesh -> glTF, texture/theme material -> PNG, the cache) against synthetic fixtures this test
# builds itself - no game file is read. Stands in for config:: and jlog:: so it does not need the ini/logging
# stack, each of which has its own selftest.
add_executable(erg_preview_selftest
    tests/erg_preview_selftest.cpp
    src/erg/preview.cpp src/erg/scene.cpp src/erg/jsonio.cpp src/erg/names.cpp src/tools/hash.cpp src/core/log.cpp)
target_include_directories(erg_preview_selftest PRIVATE src src/sdk ${stb_SOURCE_DIR})
target_compile_definitions(erg_preview_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(erg_preview_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(erg_preview_selftest PRIVATE melange_xom shell32 ole32 bcrypt)
# Level registration without the game: level-root rules, pack checks, root order, the .csh guard, the online map gate,
# the one-shot override and the "erg" channel payloads.
add_executable(levels_selftest
    tests/levels_selftest.cpp
    src/levels/roots.cpp src/levels/csh.cpp src/levels/gate_policy.cpp src/levels/live_policy.cpp src/levels/test_arm.cpp src/levels/manifest.cpp
    src/erg/names.cpp src/erg/scene.cpp src/erg/patch.cpp src/erg/jsonio.cpp src/erg/luagen.cpp
    src/assets/crcsafe_math.cpp
    src/mods/spice.cpp
    src/tools/hash.cpp
    src/tools/json_read.cpp
    src/core/log.cpp)
target_include_directories(levels_selftest PRIVATE src src/sdk)
target_compile_definitions(levels_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(levels_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(levels_selftest PRIVATE melange_xom bcrypt)
# The generated level chunk (luagen) and the pack writer (pack) over synthetic scenes and a scratch temp dir;
# also checks that dist\Mods\erg-sample parses and applies as a real spice.json + erg-patch/1 would.
add_executable(erg_pack_selftest
    tests/erg_pack_selftest.cpp
    src/erg/pack.cpp src/erg/luagen.cpp src/erg/names.cpp src/erg/scene.cpp src/erg/patch.cpp src/erg/jsonio.cpp
    src/levels/manifest.cpp src/mods/spice.cpp
    src/tools/json_read.cpp src/core/log.cpp)
target_include_directories(erg_pack_selftest PRIVATE src src/sdk)
target_compile_definitions(erg_pack_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELANGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(erg_pack_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(erg_pack_selftest PRIVATE melange_xom)
# Terrain sculpting without the game: voxel word rules, voxels ops applied and diffed over synthetic frames, the
# editor's sculpt fixture under the server's rules, and the shadow-cache deletion.
add_executable(erg_voxels_selftest
    tests/erg_voxels_selftest.cpp
    src/erg/voxels.cpp src/erg/scene.cpp src/erg/patch.cpp src/erg/names.cpp src/erg/jsonio.cpp)
target_include_directories(erg_voxels_selftest PRIVATE src src/sdk)
target_compile_definitions(erg_voxels_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELANGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(erg_voxels_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(erg_voxels_selftest PRIVATE melange_xom)

# xomtool: the Sieve CLI (unpack/pack/inspect/diff/convert/bank/report). PNG I/O only here -
# src/xom itself stays platform-free.
add_executable(xomtool tools/xomtool/main.cpp tools/xomtool/level.cpp ${ERG_SERVICE_SOURCES})
target_include_directories(xomtool PRIVATE src/xom src src/sdk ${stb_SOURCE_DIR})
target_compile_definitions(xomtool PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(xomtool PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(xomtool PRIVATE melange_xom bcrypt shell32 ole32)

# The Erg level service without the game: synthetic .xan/.XOM documents through load, patch, build (renumbering,
# byte-identical empty builds), the registry bank, the project store and every level.* method over a temp install.
add_executable(erg_level_selftest tests/erg_level_selftest.cpp ${ERG_SERVICE_SOURCES})
target_include_directories(erg_level_selftest PRIVATE src src/sdk ${stb_SOURCE_DIR})
target_compile_definitions(erg_level_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(erg_level_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(erg_level_selftest PRIVATE melange_xom bcrypt shell32 ole32)

# Image/mesh/glTF/bank conversions without the game: round trips against real shipped files
# and a reference bank, run with --game pointing at an installed copy (skipped otherwise).
add_executable(xom_convert_selftest tests/xom_convert_selftest.cpp)
target_include_directories(xom_convert_selftest PRIVATE src/xom ${stb_SOURCE_DIR})
target_compile_definitions(xom_convert_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(xom_convert_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(xom_convert_selftest PRIVATE melange_xom)

# The clone registry against a fake engine: creation per match, cells, text, icons, the slot swap and the vid mapping.
add_executable(weapons_registry_selftest
    tests/weapons_registry_selftest.cpp
    src/weapons/registry_core.cpp
    src/weapons/manifest.cpp
    src/weapons/schema.cpp
    src/mods/spice.cpp
    src/tools/json_read.cpp
    src/core/log.cpp)
target_include_directories(weapons_registry_selftest PRIVATE src src/sdk)
target_compile_definitions(weapons_registry_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(weapons_registry_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(weapons_registry_selftest PRIVATE melange_xom)

# Clone behaviour without the game: event observers, the extra-explosion window and limits, and the melange.weapons
# Wormsign contributor over fake containers.
add_executable(weapons_behaviour_selftest
    tests/weapons_behaviour_selftest.cpp
    src/weapons/behaviour_core.cpp
    src/weapons/contrib.cpp
    src/weapons/manifest.cpp
    src/weapons/schema.cpp
    src/lua/sim/tweak.cpp
    src/lua/engine50.cpp
    src/mods/spice.cpp
    src/tools/json_read.cpp
    src/core/mem.cpp
    src/core/game.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/config_schema.cpp
    src/core/module.cpp)
target_include_directories(weapons_behaviour_selftest PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(weapons_behaviour_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(weapons_behaviour_selftest PRIVATE /W4 /utf-8 /permissive- /wd4100)
target_link_options(weapons_behaviour_selftest PRIVATE /SAFESEH:NO)
target_link_libraries(weapons_behaviour_selftest PRIVATE safetyhook::safetyhook melange_xom bcrypt psapi)

# Mod-asset safety without the game: CRC-collision matching, the loose-root naming rule, bank path safety/size cap,
# and the panel-icon pixel math. Each source here is the pure half of a pair split from its engine-touching sibling
# (crcsafe.cpp, roots.cpp, banks_load.cpp, icons.cpp), which only the game can exercise.
add_executable(assets_selftest
    tests/assets_selftest.cpp
    src/assets/crcsafe_math.cpp
    src/assets/crcsafe_file.cpp
    src/assets/roots_math.cpp
    src/assets/banks.cpp
    src/assets/icons_math.cpp)
target_include_directories(assets_selftest PRIVATE src src/sdk)
target_compile_definitions(assets_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(assets_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(assets_selftest PRIVATE melange_xom)

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
# The Sandbox without the game: Thumper, the overlay, draw, config and jlog are faked in the test.
add_executable(sandbox_selftest
    tests/sandbox_selftest.cpp
    src/lua/sandbox.cpp src/lua/sandbox_limits.cpp
    src/lua/wum_core.cpp src/lua/wum_ui.cpp src/lua/wum_render.cpp src/lua/wum_unsafe.cpp
    src/tools/json_read.cpp)
target_include_directories(sandbox_selftest PRIVATE src src/sdk)
target_compile_definitions(sandbox_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELANGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sandbox_selftest PRIVATE /W4 /utf-8 /permissive- /EHsc /wd4100)
target_link_libraries(sandbox_selftest PRIVATE lua54 imgui)
# Stock Lua 5.0.1 with float numbers, a stand-in for the engine's match VM in sim_selftest (never linked into the plugin).
FetchContent_Declare(lua50
    URL https://www.lua.org/ftp/lua-5.0.1.tar.gz
    URL_HASH SHA256=7a09d0e70dcaff7feae97cf9c154da05b1e5b92eaea2df7150b54bcaf8f3b9c6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR not-a-cmake-project)
FetchContent_MakeAvailable(lua50)
file(GLOB LUA50_SOURCES ${lua50_SOURCE_DIR}/src/*.c ${lua50_SOURCE_DIR}/src/lib/*.c)
list(FILTER LUA50_SOURCES EXCLUDE REGEX "/(ltests|liolib|loadlib)\\.c$")
add_library(lua50_ref STATIC EXCLUDE_FROM_ALL ${LUA50_SOURCES})
target_include_directories(lua50_ref PUBLIC ${lua50_SOURCE_DIR}/include PRIVATE ${lua50_SOURCE_DIR}/src)
target_compile_definitions(lua50_ref PUBLIC LUA_NUMBER=float PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(lua50_ref PRIVATE /W0 /FI${CMAKE_SOURCE_DIR}/tests/lua50_noopt.h)

add_executable(sim_selftest EXCLUDE_FROM_ALL
    tests/sim_selftest.cpp
    src/lua/sim/sim_env.cpp src/lua/sim/sim_api.cpp src/lua/sim/sim_events.cpp src/lua/sim/sim_rng.cpp
    src/lua/sim/sim_hash.cpp src/lua/sim/sim_weapons.cpp src/lua/sim/sim_level.cpp src/weapons/behaviour_core.cpp
    src/wormsign/contrib.cpp
    src/core/jlog.cpp src/core/jlog_ring.cpp src/core/jlog_bus_filter.cpp
    src/core/events.cpp src/core/game.cpp src/core/mem.cpp src/core/log.cpp)
target_include_directories(sim_selftest PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(sim_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELANGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sim_selftest PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(sim_selftest PRIVATE lua50_ref bcrypt psapi shell32 ole32)

# dist/Mods/mega-bazooka (component F): the spice.json weapons array parses, sim/main.lua compiles under the
# real Lua 5.0 dialect, and the shipped icon/HUD art matches the manifest's own size and format rules.
add_executable(mega_bazooka_selftest EXCLUDE_FROM_ALL
    tests/mega_bazooka_selftest.cpp
    src/weapons/manifest.cpp
    src/weapons/schema.cpp
    src/mods/spice.cpp
    src/tools/json_read.cpp
    src/core/log.cpp)
target_include_directories(mega_bazooka_selftest PRIVATE src src/sdk ${stb_SOURCE_DIR})
target_compile_definitions(mega_bazooka_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    MELANGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(mega_bazooka_selftest PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(mega_bazooka_selftest PRIVATE melange_xom lua50_ref)

# Offline self-tests (no game needed).
add_executable(bus_selftest EXCLUDE_FROM_ALL
    tests/bus_selftest.cpp
    src/core/bus.cpp src/core/bus_registry.cpp src/core/bus_decoders.cpp
    src/core/events.cpp src/core/game.cpp src/core/mem.cpp src/core/log.cpp src/core/config.cpp src/core/config_schema.cpp
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

add_executable(console_selftest EXCLUDE_FROM_ALL
    tests/console_selftest.cpp
    src/lua/console_repl.cpp
    src/lua/console_history.cpp)
target_include_directories(console_selftest PRIVATE src)
target_compile_options(console_selftest PRIVATE /W4 /utf-8 /permissive-)

add_executable(jlog_selftest
    tests/jlog_selftest.cpp
    src/core/jlog.cpp
    src/core/jlog_ring.cpp
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

# Weapon tweaks without the game: schema lookup, snapshot/restore and the unknown-build guard.
add_executable(tweak_selftest
    tests/tweak_selftest.cpp
    src/lua/sim/tweak.cpp
    src/lua/engine50.cpp
    src/core/mem.cpp
    src/core/game.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/config_schema.cpp
    src/core/module.cpp)
target_include_directories(tweak_selftest PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(tweak_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(tweak_selftest PRIVATE /W4 /utf-8 /permissive- /Zi /wd4100)
target_link_options(tweak_selftest PRIVATE /DEBUG /SAFESEH:NO)
target_link_libraries(tweak_selftest PRIVATE safetyhook::safetyhook bcrypt psapi)

# The .wsr recording container without the game: round trips, truncation, bad CRCs and a timed reader mutation run.
add_executable(wormsign_format_selftest
    tests/wormsign_format_selftest.cpp
    src/wormsign/format.cpp
    src/tools/json_read.cpp)
target_include_directories(wormsign_format_selftest PRIVATE src src/sdk)
target_compile_definitions(wormsign_format_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(wormsign_format_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(wormsign_format_selftest PRIVATE miniz)

# Hash contributors, detail records and the FPU watch without the game: registry order, demotion, faults, the detail
# codec (with a timed mutation run) and the FPU change log.
add_executable(wormsign_contrib_selftest
    tests/wormsign_contrib_selftest.cpp
    src/wormsign/contrib.cpp src/wormsign/detail.cpp src/wormsign/fpu.cpp
    src/tools/json_read.cpp
    src/core/jlog.cpp src/core/jlog_ring.cpp src/core/jlog_bus_filter.cpp
    src/core/events.cpp src/core/game.cpp src/core/mem.cpp src/core/log.cpp)
target_include_directories(wormsign_contrib_selftest PRIVATE src src/sdk ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(wormsign_contrib_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(wormsign_contrib_selftest PRIVATE /W4 /utf-8 /permissive- /wd4100)
target_link_libraries(wormsign_contrib_selftest PRIVATE bcrypt psapi shell32 ole32)

# The rolling recorder's writer and library without the game: the writer thread, retention, pin/flag exemption and
# the export redaction policy, all against synthetic recordings.
add_executable(wormsign_recorder_selftest
    tests/wormsign_recorder_selftest.cpp
    src/wormsign/format.cpp
    src/wormsign/writer.cpp
    src/wormsign/library.cpp
    src/wormsign/records.cpp
    src/tools/json_read.cpp
    src/tools/redact.cpp
    src/tools/hash.cpp
    src/core/game.cpp
    src/core/log.cpp)
target_include_directories(wormsign_recorder_selftest PRIVATE src src/sdk)
target_compile_definitions(wormsign_recorder_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(wormsign_recorder_selftest PRIVATE /W4 /utf-8 /permissive- /wd4100)
target_link_libraries(wormsign_recorder_selftest PRIVATE miniz bcrypt psapi shell32 ole32)

# The desync detector without the game: the exchange packets, the two-peer protocol over a simulated link, the
# field diff and the bundle, plus a timed mutation run on the packet decoder.
add_executable(wormsign_detector_selftest
    tests/wormsign_detector_selftest.cpp
    src/wormsign/exchange_wire.cpp
    src/wormsign/detector_core.cpp
    src/wormsign/bundle.cpp
    src/tools/json_read.cpp
    src/tools/redact.cpp
    src/tools/hash.cpp)
target_include_directories(wormsign_detector_selftest PRIVATE src src/sdk)
target_compile_definitions(wormsign_detector_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(wormsign_detector_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(wormsign_detector_selftest PRIVATE miniz bcrypt)

# The replay player without the game: record payloads, loading, arm checks, forcing, the input schedule, compare,
# the virtual clock, the setup fingerprint and a record/replay round trip on a toy simulation.
add_executable(wormsign_player_selftest
    tests/wormsign_player_selftest.cpp
    src/wormsign/format.cpp
    src/wormsign/records.cpp
    src/wormsign/recording.cpp
    src/wormsign/replay_core.cpp
    src/wormsign/setup.cpp
    src/tools/json_read.cpp)
target_include_directories(wormsign_player_selftest PRIVATE src src/sdk)
target_compile_definitions(wormsign_player_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(wormsign_player_selftest PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(wormsign_player_selftest PRIVATE miniz)
