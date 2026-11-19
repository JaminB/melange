#pragma once
// The game's speech banks on disk (<GameDir>\Data\Audio\PC\vo*.fsb) for wum.audio.voice (Melange 0.17). A bank's
// header is read once and kept; a line's data is read from the file when it is loaded. Main thread only.
#include <cstdint>
#include <string>
#include <vector>

#include "audio/fsb_voice.h"

namespace melange::audio::voicebanks {
// The parsed bank, or nullptr with *why = "unknown bank" (bad name, no such file) or "unreadable bank". `bank` is
// matched against the file names case-insensitively; *canonical is the lower-case name used for cache keys.
const fsb::Bank* Get(const std::string& bank, std::string* canonical, std::string* why);
// Reads and decodes one sample of a bank returned by Get.
bool Decode(const std::string& canonical, const fsb::Sample& s, fsb::Pcm* out, std::string* why);
}  // namespace melange::audio::voicebanks
