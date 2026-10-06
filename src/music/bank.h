#pragma once
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

// The music module's data side: an MP3 frame scanner and the FSB4 writer for the sudden-death bank. Pure functions
// with no engine dependencies (music.cpp reads the files and talks to the game; tests/music_selftest.cpp links this).
namespace melange::music {
constexpr size_t kMaxFileBytes = 24u << 20;         // one MP3
constexpr size_t kMaxPerMod = 16, kMaxPerSlot = 64; // entries per mod, tracks per slot across enabled mods
constexpr uint32_t kMinFrames = 50;
constexpr size_t kHeaderBytes = 48, kSampleHeaderBytes = 80;
constexpr size_t kNoTrack = static_cast<size_t>(-1);

// The MPEG audio frames of one file, concatenated (tags and junk dropped).
struct Track {
    std::vector<uint8_t> frames;
    uint32_t frameCount = 0, samples = 0, sampleRate = 0;
    int channels = 0;                                // 1 or 2
};
struct Scan {
    bool ok = false;
    std::string error;                               // why a refused file was refused
    Track track;
};

// Skips an ID3v2 tag, then scans for MPEG-1/2 Layer II/III frames, stepping over any junk between them.
// Refused: fewer than kMinFrames frames, or a sample rate or channel count that changes between frames.
Scan ScanMp3(const uint8_t* data, size_t size);

// FSB4 bank with one MPEG sample named "SuddenDeath" made of `tracks` in order. `vanilla` is the first
// kHeaderBytes of the game's own bank: its bytes 16..47 (version, mode, bank ID, hash) are copied verbatim.
// All tracks must share a sample rate and channel count.
bool CheckVanillaHeader(const uint8_t* vanilla, size_t size, std::string* err);   // FSB4 with numsamples 1
bool BuildBank(const uint8_t* vanilla, const std::vector<const Track*>& tracks, std::vector<uint8_t>* out, std::string* err);

// A uniform random order of 0..n-1. With n >= 2 and a valid `lastFirst` (the index that led last time) the first
// entry is never `lastFirst`; kNoTrack or an out-of-range value means no constraint.
std::vector<size_t> ShuffleOrder(size_t n, size_t lastFirst, std::mt19937& rng);

// True when a CreateFileA name, lowercased with '/' turned into '\', ends with "\audio\pc\musuddendeath.fsb".
bool IsSuddenDeathPath(const char* name);
}  // namespace melange::music
