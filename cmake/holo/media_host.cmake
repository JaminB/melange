# media_selftest additions for the media host: follow policy, pipe client, watchdog, upload bookkeeping (owner: C4b).
# Its cases live in tests/media_selftest_host.inc; tests/fake_helper.cpp gets its own target here. Game-free sources only.
target_sources(media_selftest PRIVATE src/media/pipe.cpp src/media/shm.cpp src/media/helper.cpp)
target_link_libraries(media_selftest PRIVATE advapi32 shell32 ole32)

# The stand-in for holo-media.exe (x86, the same protocol, synthetic frames, scripted faults). media_selftest starts it
# from its own folder, so it is built with it.
add_executable(fake_helper tests/fake_helper.cpp)
target_include_directories(fake_helper PRIVATE src)
target_compile_definitions(fake_helper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(fake_helper PRIVATE /W4 /utf-8 /permissive- /EHsc)
add_dependencies(media_selftest fake_helper)
