# The torrent engine's build pieces (C6b), included by helper/holo-media/CMakeLists.txt after the source glob.
# src/bt/tests/ holds test sources only; they never go into holo-media.exe.
list(FILTER HOLO_SOURCES EXCLUDE REGEX "/src/bt/tests/")

# holo_bt_test: the engine's pure rules and cache lifecycle always; with HOLO_TORRENT also the libtorrent engine
# against a loopback seeder (no internet, no firewall prompt: everything listens on 127.0.0.1).
set(HOLO_BT_TEST_SOURCES src/bt/tests/bt_test.cpp src/bt/bt_cache.cpp)
if(HOLO_TORRENT)
    list(APPEND HOLO_BT_TEST_SOURCES src/bt/bt_libtorrent.cpp)
endif()
if(HOLO_MPV_DIR)
    list(APPEND HOLO_BT_TEST_SOURCES src/bt/bt_mpv.cpp)
endif()
add_executable(holo_bt_test ${HOLO_BT_TEST_SOURCES})
target_include_directories(holo_bt_test PRIVATE src ${MELANGE_SRC})
target_compile_definitions(holo_bt_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(holo_bt_test PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(holo_bt_test PRIVATE advapi32 ws2_32)
if(HOLO_MPV_DIR)
    target_include_directories(holo_bt_test PRIVATE ${HOLO_MPV_DIR}/include)
    target_compile_definitions(holo_bt_test PRIVATE HOLO_HAVE_MPV=1)
endif()
if(HOLO_TORRENT)
    find_package(LibtorrentRasterbar CONFIG REQUIRED)
    target_link_libraries(holo_bt_test PRIVATE LibtorrentRasterbar::torrent-rasterbar)
    target_compile_definitions(holo_bt_test PRIVATE HOLO_HAVE_TORRENT=1)
endif()
