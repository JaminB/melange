# media_selftest additions for the pack installer, extractor, manifest rules and the yt-dlp lane (owner: C4a).
# Its cases live in tests/media_selftest_pack.inc: the pure rules (pack_logic.h), the resumable file:/// download
# (store/fetch.cpp with its Range resume), the streaming extractor and install, the sweep, uninstall and the lane.
# store/index.cpp brings SchemeOf/FileUrlToPath, which needs mods/spice.cpp.
target_sources(media_selftest PRIVATE
    src/media/pack_fetch.cpp src/media/pack_extract.cpp src/media/ytdlp_lane.cpp
    src/store/fetch.cpp src/store/index.cpp src/mods/spice.cpp)
