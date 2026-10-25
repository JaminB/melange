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
    bool music = false;  // kind="music": routed through the music submix, silenced with the game's music
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
void Release(const void* owner);               // stops the owner's voices and drops its clips, and its music request

// Game music (Melange 0.12): the mods' requests to silence it, applied to FMOD's music category (music/fmod_mute.cpp)
// and to the music submix. One request per owner, the music is silent while any owner asks.
// True once the FMOD music category was found (a request made earlier is kept and applied then).
bool MusicAvailable();
// Returns nullptr, or "unavailable" (FMOD cannot be driven in this process) or "disabled" ([Audio] AllowMusicSuppress=0).
// fade < 0 = the default 0.4 s.
const char* MusicSuppress(const void* owner, bool on, float fade);
void MusicState(const void* owner, bool* effective, bool* mine);
// 1 = audible, 0 = silent, in between while fading; advances the fade to the current time. For fmod_mute.cpp.
float MusicLevel();
}  // namespace melange::audio
