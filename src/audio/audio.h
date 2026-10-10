#pragma once
// Client-side sound for mods (module "Audio"): what wum.audio calls. Presentation only; the simulation and the wire
// never see it. "Owner" is an opaque token for one mod generation: clips and voices belong to it, and Release drops
// them when that generation is revoked (unload, reload, disable).
#include <cstddef>
#include <cstdint>
#include <string>

namespace melange::audio {
struct PlayOpts {
    float volume = 1.0f;  // 0..2
    float pitch = 1.0f;   // frequency ratio, 0.5..2
    bool loop = false;
    bool positional = false;
    float pos[3] = {0, 0, 0};
};

// True when the module is installed and the XAudio2 backend is up. The first call (or the first play) brings the
// backend up; a failure is logged once and retried after a while.
bool Available();

uint32_t FindClip(const void* owner, const std::string& key);  // 0 when not loaded
// Parses WAV bytes into the owner's clip cache. Returns the clip handle (> 0) or 0 with *why set.
uint32_t LoadClip(const void* owner, const std::string& key, const uint8_t* bytes, size_t n, std::string* why);
// Starts a clip. Returns the voice id (> 0) or 0 with *why set.
uint32_t Play(const void* owner, uint32_t clip, const PlayOpts& opts, std::string* why);
bool Stop(const void* owner, uint32_t voice);  // false if the voice is unknown, ended, or another mod's
size_t StopAll(const void* owner);             // returns how many voices were stopped
void Release(const void* owner);               // stops the owner's voices and drops its clips
}  // namespace melange::audio
