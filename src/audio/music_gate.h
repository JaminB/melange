#pragma once
// wum.audio.music.suppress without the game: which mods ask for silence, and the fade between audible and silent. One
// boolean per owner (a mod generation), the effective state is their OR, and a released owner (unload, reload,
// disable) takes its request with it so a crashed mod cannot leave the game silent. The level is 1 when the music is
// audible and 0 when it is silent; fmod_mute.cpp and the music submix follow it. Main thread.
#include <algorithm>
#include <map>

namespace melange::audio {
class MusicGate {
public:
    static constexpr float kDefaultFade = 0.4f;  // seconds
    static constexpr double kMaxStep = 0.25;     // a long gap between steps (no frames) never skips a whole fade

    // `fade` seconds (0 = at once); NaN or negative means the default. The latest request sets the speed.
    void Set(const void* owner, bool on, float fade = -1.0f) {
        fade_ = (fade >= 0.0f && fade <= 60.0f) ? fade : kDefaultFade;
        if (on) owners_[owner] = true;
        else owners_.erase(owner);
    }
    void Release(const void* owner) { owners_.erase(owner); }
    bool Wanted() const { return !owners_.empty(); }  // any mod asks
    bool Has(const void* owner) const { return owners_.count(owner) != 0; }

    // Moves the level towards the target by the time since the previous step (`now` in seconds, any origin).
    void Step(double now) {
        const double dt = started_ ? std::clamp(now - last_, 0.0, kMaxStep) : 0.0;
        started_ = true;
        last_ = now;
        const float target = Wanted() ? 0.0f : 1.0f;
        if (level_ == target) return;
        if (fade_ <= 0.0f) {
            level_ = target;
            return;
        }
        const float d = static_cast<float>(dt) / fade_;
        level_ = target < level_ ? std::max(target, level_ - d) : std::min(target, level_ + d);
    }
    float Level() const { return level_; }
    bool Silent() const { return level_ <= 0.0f; }
    bool Fading() const { return level_ > 0.0f && level_ < 1.0f; }

private:
    std::map<const void*, bool> owners_;
    float level_ = 1.0f, fade_ = kDefaultFade;
    double last_ = 0.0;
    bool started_ = false;
};
}  // namespace melange::audio
