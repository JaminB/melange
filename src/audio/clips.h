#pragma once
// One mod's decoded sounds. A clip is parsed once and kept (as shared PCM) until the owner is dropped; a playing voice
// holds its own reference, so the buffer XAudio2 reads asynchronously is never freed under it. Pure: no Windows.
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace melange::audio {
constexpr size_t kMaxClipsPerMod = 64;
constexpr size_t kMaxPcmPerMod = 16u << 20;

// The cache key for a mod-relative path: separators unified, "." and empty segments dropped, ASCII case folded. Windows
// resolves "sfx/boom.wav", "SFX\Boom.wav" and "sfx/./boom.wav" to one file, and it should cost one clip, not three.
std::string NormalizeClipKey(const std::string& rel);

struct Clip {
    uint32_t id = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint32_t frames = 0;
    std::vector<uint8_t> pcm;  // 16-bit interleaved
};

class ClipCache {
public:
    explicit ClipCache(size_t maxClips = kMaxClipsPerMod, size_t maxBytes = kMaxPcmPerMod)
        : maxClips_(maxClips), maxBytes_(maxBytes) {}

    uint32_t Find(const std::string& key) const;  // 0 when not loaded
    // Parses the WAV bytes and stores the clip under `key`; the same key again returns the same id. Returns 0 and sets
    // *why when the file is not a usable WAV or a per-mod limit would be exceeded.
    uint32_t Add(const std::string& key, const uint8_t* bytes, size_t n, std::string* why);
    std::shared_ptr<const Clip> Get(uint32_t id) const;
    size_t Count() const { return clips_.size(); }
    size_t Bytes() const { return bytes_; }

private:
    size_t maxClips_, maxBytes_;
    size_t bytes_ = 0;
    uint32_t next_ = 0;
    std::map<std::string, uint32_t> byKey_;
    std::map<uint32_t, std::shared_ptr<const Clip>> clips_;
};
}  // namespace melange::audio
