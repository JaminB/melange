#pragma once
// RIFF/WAVE reader for mod sounds. Pure: no Windows, no engine, so the self-test covers it.
#include <cstddef>
#include <cstdint>
#include <string>

namespace melange::audio {
constexpr size_t kMaxWavBytes = 2u << 20;  // a mod sound effect, not music: bounds memory and the read
constexpr uint32_t kMinRate = 8000, kMaxRate = 48000;

struct WavInfo {
    uint16_t channels = 0;       // 1 or 2
    uint32_t rate = 0;           // Hz
    uint32_t frames = 0;         // samples per channel
    const uint8_t* data = nullptr;  // 16-bit little-endian interleaved PCM, points into the input
    size_t bytes = 0;            // frames * channels * 2
};

// Accepts PCM (format 1) or WAVE_FORMAT_EXTENSIBLE whose subformat is PCM, 16 bits, 1 or 2 channels, 8000-48000 Hz.
// Other chunks are skipped. On failure returns false and sets *why to a short reason fit for a Lua error string.
bool ParseWav(const uint8_t* p, size_t n, WavInfo* out, std::string* why);
}  // namespace melange::audio
