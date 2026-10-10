// Audio: sound playback for mods. Pure parts are wav.cpp, clips.cpp, attenuation.h and voicebook.h; mixer.cpp is the
// XAudio2 side. This file ties them to the game: the module, the per-frame voice upkeep, and the facade (audio.h) that
// wum.audio in src/lua/wum_audio.cpp calls.
#include "audio/audio.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "audio/attenuation.h"
#include "audio/clips.h"
#include "audio/mixer.h"
#include "audio/voicebook.h"
#include "core/events.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/jlog.h"
#include "melange/render.h"
#include "melange/sim.h"

namespace melange::audio {
namespace {
constexpr uint64_t kRetryFrames = 600;  // after the backend failed to come up, try again no sooner than this

bool g_installed = false;
float g_master = 1.0f, g_near = 150.0f, g_far = 1500.0f;
bool g_wasInMatch = false;

// Never destroyed by a static destructor: tearing XAudio2 down during process exit can hang. The one teardown is
// Audio::Uninstall, which runs only when the DLL is unloaded with FreeLibrary; a normal process exit leaves the engine
// to the OS.
Mixer* g_mixer = nullptr;
VoiceBook g_book;
std::map<const void*, ClipCache> g_clips;

bool g_failed = false;
uint64_t g_failedAt = 0;
std::string g_lastFailure;

bool Ensure() {
    if (!g_installed) return false;
    if (g_mixer && g_mixer->Ready()) return true;
    const uint64_t now = events::FrameCount();
    if (g_failed && now - g_failedAt < kRetryFrames) return false;
    if (!g_mixer) g_mixer = new Mixer();
    std::string why;
    if (g_mixer->Init(&why)) {
        g_mixer->SetMasterVolume(g_master);
        g_failed = false;
        LOG_INFO("[audio] XAudio2 started (master volume %.2f, up to %zu voices, near %.0f far %.0f)", g_master, g_book.Max(),
                 g_near, g_far);
        jlog::Rec("audio", jlog::Level::Info, "started").Float("volume", g_master).Uint("maxVoices", g_book.Max());
        return true;
    }
    g_failed = true;
    g_failedAt = now;
    if (why != g_lastFailure) {  // once per distinct reason, not once per retry
        g_lastFailure = why;
        LOG_WARN("[audio] unavailable: %s; wum.audio.play returns nil", why.c_str());
        jlog::Rec("audio", jlog::Level::Warn, "unavailable").Str("why", why);
    }
    return false;
}

size_t StopVoices(const std::vector<uint32_t>& ids) {
    size_t n = 0;
    for (uint32_t id : ids) {
        if (g_mixer) g_mixer->Stop(id);
        if (g_book.Remove(id)) ++n;
    }
    return n;
}

// Voices started in the current frame are spared: a mod's melange.match.end handler runs in the same frame as the
// transition seen here, and the sound it starts there is meant to be heard.
void StopEverything(const char* why) {
    const size_t n = StopVoices(g_book.StartedBefore(events::FrameCount()));
    if (n) {
        LOG_INFO("[audio] %zu voice(s) stopped: %s", n, why);
        jlog::Rec("audio", jlog::Level::Info, "stop_all").Str("why", why).Uint("voices", n);
    }
}

void OnFrame() {
    if (!g_installed) return;
    // Offline matches do not fire MatchEnd, but their match VM goes away; online ones do both, which is harmless.
    const bool inMatch = sim::InMatch();
    if (g_wasInMatch && !inMatch) StopEverything("the match ended");
    g_wasInMatch = inMatch;

    if (!g_mixer) return;
    std::vector<uint32_t> ended;
    g_mixer->Poll(&ended);
    for (uint32_t id : ended) g_book.Remove(id);
    if (!g_mixer->Ready() || g_book.Count() == 0) return;

    // Positional voices follow the camera. Without a camera (Mirage off, or before the first main pass) they stay at
    // the level they were started with.
    bool any = false;
    for (const auto& [id, v] : g_book.Voices())
        if (v.positional) any = true;
    if (!any) return;
    render::Camera cam;
    if (!render::GetCamera(&cam) || !cam.valid) return;
    for (auto& [id, v] : g_book.Voices()) {
        if (!v.positional) continue;
        const float level = PositionalVolume(v.volume, Distance(cam.pos, v.pos), g_near, g_far);
        if (std::fabs(level - v.applied) < 0.002f) continue;
        v.applied = level;
        g_mixer->SetVolume(id, level);
    }
}

class Audio final : public Module {
public:
    const char* Name() const override { return "Audio"; }
    const char* Description() const override { return "sound playback for mods (wum.audio)"; }
    bool Install() override {
        const float v = Float("Volume", 1.0f);
        g_master = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
        g_book.SetMax(static_cast<size_t>(std::clamp(Int("MaxVoices", 32), 1, 64)));
        g_near = std::max(0.0f, Float("Near", 150.0f));
        g_far = Float("Far", 1500.0f);
        g_installed = true;
        // XAudio2 is not touched until a mod asks for a sound (or checks wum.audio.ready()).
        events::Subscribe(events::Event::Frame, [] { OnFrame(); });
        events::Subscribe(events::Event::MatchEnd, [] { StopEverything("the match ended"); });
        LOG_INFO("[audio] installed (backend starts on the first sound)");
        return true;
    }
    // Reached only from DllMain on FreeLibrary (Event::Shutdown is never fired). The engine thread has to stop before
    // this DLL's code goes away, so the blocking teardown is done even though DllMain is not the place for it.
    void Uninstall() override {
        g_installed = false;
        if (g_mixer) g_mixer->Shutdown();
    }
};
}  // namespace

MELANGE_MODULE(Audio);

bool Available() { return Ensure(); }

uint32_t FindClip(const void* owner, const std::string& key) {
    auto it = g_clips.find(owner);
    return it == g_clips.end() ? 0 : it->second.Find(key);
}

uint32_t LoadClip(const void* owner, const std::string& key, const uint8_t* bytes, size_t n, std::string* why) {
    return g_clips[owner].Add(key, bytes, n, why);
}

uint32_t Play(const void* owner, uint32_t clipId, const PlayOpts& opts, std::string* why) {
    if (!Ensure()) {
        if (why) *why = "audio unavailable";
        return 0;
    }
    auto it = g_clips.find(owner);
    const std::shared_ptr<const Clip> clip = it == g_clips.end() ? nullptr : it->second.Get(clipId);
    if (!clip) {
        if (why) *why = "unknown sound";
        return 0;
    }
    // The book only knows ids; the mixer's count is the one that also covers a voice it failed to start earlier. Stopped
    // voices awaiting release are not in it (the mixer bounds those on its own), so stop() then play() always has room.
    if (g_book.Full() || g_mixer->Active() >= g_book.Max()) {
        if (why) *why = "too many voices";
        return 0;
    }
    VoiceInfo vi;
    vi.owner = owner;
    vi.volume = opts.volume;
    vi.positional = opts.positional;
    std::copy(opts.pos, opts.pos + 3, vi.pos);
    float level = opts.volume;
    if (opts.positional) {
        render::Camera cam;
        if (render::GetCamera(&cam) && cam.valid) level = PositionalVolume(opts.volume, Distance(cam.pos, vi.pos), g_near, g_far);
        // A NaN position is silent whether or not a camera exists to measure the distance from (the docs promise it).
        if (std::isnan(vi.pos[0]) || std::isnan(vi.pos[1]) || std::isnan(vi.pos[2])) level = 0.0f;
    }
    vi.applied = level;
    vi.startFrame = events::FrameCount();
    const uint32_t id = g_book.Add(vi);
    if (!id) {
        if (why) *why = "too many voices";
        return 0;
    }
    if (!g_mixer->Start(id, clip, level, opts.pitch, opts.loop, why)) {
        g_book.Remove(id);
        return 0;
    }
    return id;
}

bool Stop(const void* owner, uint32_t voice) {
    if (!g_book.Owns(owner, voice)) return false;
    return StopVoices({voice}) != 0;
}

size_t StopAll(const void* owner) { return StopVoices(g_book.OwnedBy(owner)); }

void Release(const void* owner) {
    StopAll(owner);
    g_clips.erase(owner);
}
}  // namespace melange::audio
