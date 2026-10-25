# holo-media.exe: the x64 media helper (mpv playback, yt-dlp resolve, torrents) that melange.asi starts out of process.
# Its own project: the root CMakeLists.txt is x86 only. Build with helper\holo-media\build.ps1 (vcvars64).
# The protocol headers are shared with the core and included read-only from ../../src/media (pipe_codec.h,
# shm_logic.h, url_logic.h, media_proto.h); this project never copies or edits them.
cmake_minimum_required(VERSION 3.25)
project(HoloMedia VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "holo-media must be built for x64 (it runs as a separate 64-bit process). Use build.ps1.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the pack needs no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(MELANGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# libmpv headers (include/mpv/client.h, render.h, stream_cb.h) from the pinned dev zip. libmpv-2.dll itself is
# loaded at run time by full path (--libmpv), so nothing links against it. Without it the build has no player (C6a's
# code compiles its mpv parts under HOLO_HAVE_MPV).
set(HOLO_MPV_DIR "" CACHE PATH "libmpv dev folder holding include/mpv/*.h")
# The libtorrent engine (vcpkg, static). OFF builds src/bt/bt_none.cpp, which reports "no torrent support".
option(HOLO_TORRENT "Build the libtorrent engine (needs the vcpkg toolchain file)" OFF)

file(GLOB_RECURSE HOLO_SOURCES CONFIGURE_DEPENDS src/*.cpp)
if(HOLO_TORRENT)
    list(FILTER HOLO_SOURCES EXCLUDE REGEX "/src/bt/bt_none[.]cpp$")
else()
    list(FILTER HOLO_SOURCES EXCLUDE REGEX "/src/bt/bt_libtorrent[^/]*[.]cpp$")
endif()
# The torrent engine's own build pieces (test target, test-source filter): src/bt is C6b's.
include(src/bt/bt.cmake OPTIONAL)

add_executable(holo-media ${HOLO_SOURCES})
target_include_directories(holo-media PRIVATE src ${MELANGE_SRC})
target_compile_definitions(holo-media PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
    HOLO_MEDIA_VERSION="${PROJECT_VERSION}")
target_compile_options(holo-media PRIVATE /W4 /utf-8 /permissive- /Zi /EHsc)
target_link_options(holo-media PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(holo-media PRIVATE advapi32 userenv shell32 ws2_32 bcrypt)
if(HOLO_MPV_DIR)
    target_include_directories(holo-media PRIVATE ${HOLO_MPV_DIR}/include)
    target_compile_definitions(holo-media PRIVATE HOLO_HAVE_MPV=1)
endif()
if(HOLO_TORRENT)
    find_package(LibtorrentRasterbar CONFIG REQUIRED)
    target_link_libraries(holo-media PRIVATE LibtorrentRasterbar::torrent-rasterbar)
    target_compile_definitions(holo-media PRIVATE HOLO_HAVE_TORRENT=1)
endif()

# The shared protocol headers compiled for x64: the same layout and codec cases the core's media_selftest runs on x86.
add_executable(holo_media_shared_test tests/shared_headers_test.cpp)
target_include_directories(holo_media_shared_test PRIVATE ${MELANGE_SRC})
target_compile_options(holo_media_shared_test PRIVATE /W4 /utf-8 /permissive- /EHsc)
