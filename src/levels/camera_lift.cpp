// LevelCamera: the drawn camera never rests inside the land on a pack or Test level (Erg).
//
// The game's cameras keep their eye where the game's own maps leave room. On Caldera (2026-11-10, testenv B, before
// the crater floor got its slab) a Fatkins payload that bored through the hollow volcano's shell drowned in the sea
// inside it, and the game's drown camera then held the eye at world y 144 inside the cone, looking at that sea, for
// about 8 s (3 strikes of 3). The same cameras can pass through the cone between turns for a frame or two. On the
// game's own maps those points are over open sea or low islands, so nothing shows it.
//
// The fix is render-only: once a frame, inside the engine's Render (MirageCamera's hook, camoverride.cpp), the game's
// eye column is swept straight down from high above with gamestate::LandRay (which saves and restores every global it
// touches, so the simulation never sees it). Land over the eye is not enough to move it: Caldera's castles have roofed
// courts where a turn camera sits under the roof (Ashgate: 167 world units under the land's top) and sees its worm
// well. The eye is moved only when it is inside solid land (a sweep that starts there hits at once) or more than
// `MinDepth` under the land's top (logic::InLand). Then, when the camera's target is in the open, the eye moves in
// along the target->eye line to `Clearance` short of the land (logic::PullIn: the same view of the target, from
// closer); otherwise (the target is under the land too, or the pulled-in eye would be) the camera's rig moves up by its
// target's own depth under the land and the eye is kept `Clearance` over the land (logic::Lift). That frame is drawn
// from there. The game's logical camera (CameraManagerService, which Wormsign checks) and everything outside Render
// keep the game's values, so nothing a match's outcome depends on changes and a multiplayer match stays in step
// whoever has it on. It acts only while a map from a pack or an Erg Test plays (LevelStart source not vanilla) and
// does nothing (not even the sweep) on the game's own maps. Cost: one sweep a frame, up to five while the eye is under
// land.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/gamestate.h"
#include "melange/levels.h"
#include "melange/sim.h"
#include "melange/testcmd.h"
#include "render/mirage/camoverride.h"
#include "render/mirage/camoverride_logic.h"

namespace melange::levels::camlift {
namespace {
constexpr float kTop = 3000.f;  // world y the column is swept from (Erg's tallest land is far below it)

// Main thread only.
bool g_on = false;
float g_clearance = 30.f, g_minDepth = 300.f;
int g_start = 0;
struct Episode {  // a run of frames drawn lifted, for the log
    bool open = false;
    uint64_t first = 0, last = 0;
    float maxDepth = 0.f;
    float eye[3] = {}, target[3] = {}, drawn[3] = {}, ground = 0.f;
    uint64_t pulled = 0, lifted = 0;  // frames moved in towards the target / lifted over the land
} g_ep;
uint64_t g_frames = 0, g_episodes = 0;
int g_logged = 0;
constexpr int kMaxLogged = 60;

float GroundAt(float x, float z, float below) {
    gamestate::LandHit h{};
    if (gamestate::LandRay({x, kTop, z}, {x, below, z}, &h) != gamestate::LandRayResult::Hit) return NAN;
    return kTop + (below - kTop) * h.t;
}

void CloseEpisode() {
    if (!g_ep.open) return;
    g_ep.open = false;
    if (g_logged++ < kMaxLogged)
        LOG_INFO("[levels] camera out of the land for %llu frames (%llu moved in, %llu lifted): eye (%.0f, %.0f, %.0f) was "
                 "%.0f under the land's top %.0f, target (%.0f, %.0f, %.0f), drawn from (%.0f, %.0f, %.0f)",
                 static_cast<unsigned long long>(g_ep.last - g_ep.first + 1), static_cast<unsigned long long>(g_ep.pulled),
                 static_cast<unsigned long long>(g_ep.lifted), g_ep.eye[0], g_ep.eye[1], g_ep.eye[2], g_ep.maxDepth,
                 g_ep.ground, g_ep.target[0], g_ep.target[1], g_ep.target[2], g_ep.drawn[0], g_ep.drawn[1], g_ep.drawn[2]);
}

namespace lg = mirage::camoverride::logic;

// The first land on the target->eye line, as a fraction of it from the target (NaN: none). The sweep starts kSkip
// along the line so the ground the target stands on is not taken for the land behind it.
constexpr float kSkip = 10.f;
float LineHit(const float target[3], const float eye[3]) {
    const float d[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > kSkip * 2.f)) return NAN;
    const float k = kSkip / len;
    gamestate::LandHit h{};
    if (gamestate::LandRay({target[0] + d[0] * k, target[1] + d[1] * k, target[2] + d[2] * k}, {eye[0], eye[1], eye[2]}, &h) !=
        gamestate::LandRayResult::Hit)
        return NAN;
    return k + (1.f - k) * h.t;
}

// A sweep that starts inside solid land hits at once (on Caldera, 2026-11-11: t = 0 in every direction).
bool InsideSolid(const float p[3]) {
    gamestate::LandHit h{};
    return gamestate::LandRay({p[0], p[1], p[2]}, {p[0], p[1] + 40.f, p[2]}, &h) == gamestate::LandRayResult::Hit && h.t * 40.f < 0.5f;
}

// Test verb `camera.lift probe x y z tx ty tz [frames] [raw]` (world units): for `frames` frames (default 120) the
// correction takes this pose as the game's camera, so a test can draw what it does with a pose the game seldom picks
// (raw 1: draw the pose itself, uncorrected). `camera.lift` alone logs the counts.
struct Probe {
    int frames = 0;
    bool raw = false;
    float eye[3] = {}, target[3] = {};
} g_probe;

bool Correct(const float gameEye[3], const float gameTarget[3], const float*, float outEye[3], float outTarget[3]) {
    if (!g_on || !sim::InMatch()) return false;
    const float* eye = gameEye;
    const float* target = gameTarget;
    if (g_probe.frames > 0) {
        --g_probe.frames;
        eye = g_probe.eye, target = g_probe.target;
        if (g_probe.raw) {
            for (int i = 0; i < 3; ++i) outEye[i] = eye[i], outTarget[i] = target[i];
            return true;
        }
    }
    const uint64_t now = events::FrameCount();
    if (g_ep.open && now > g_ep.last + 1) CloseEpisode();
    if (!std::isfinite(eye[1]) || eye[1] >= kTop || !std::isfinite(target[1])) return false;
    const float ground = GroundAt(eye[0], eye[2], eye[1]);
    if (!lg::Buried(eye[1], ground)) return false;
    if (!lg::InLand(eye[1], ground, InsideSolid(eye), g_minDepth)) return false;
    const float groundTarget = target[1] < kTop ? GroundAt(target[0], target[2], target[1]) : NAN;
    bool pulled = false;
    if (!lg::Buried(target[1], groundTarget)) {
        // The target is in the open (a worm in a castle court, the eye in the wall behind it): keep the game's view of
        // it from closer, unless the pulled-in eye would still be under the land (an overhang between them).
        const float t = LineHit(target, eye);
        float e[3];
        if (lg::PullIn(eye, target, t, g_clearance, e) && !lg::InLand(e[1], GroundAt(e[0], e[2], e[1]), InsideSolid(e), g_minDepth)) {
            for (int i = 0; i < 3; ++i) outEye[i] = e[i], outTarget[i] = target[i];
            pulled = true;
        }
    }
    if (!pulled && !lg::Lift(eye, target, ground, groundTarget, g_clearance, outEye, outTarget)) return false;
    ++g_frames;
    if (!g_ep.open) {
        g_ep = Episode{};
        g_ep.open = true;
        g_ep.first = now;
        ++g_episodes;
    }
    g_ep.last = now;
    ++(pulled ? g_ep.pulled : g_ep.lifted);
    if (ground - eye[1] > g_ep.maxDepth) {
        g_ep.maxDepth = ground - eye[1];
        g_ep.ground = ground;
        for (int i = 0; i < 3; ++i) g_ep.eye[i] = eye[i], g_ep.target[i] = target[i], g_ep.drawn[i] = outEye[i];
    }
    return true;
}

bool Verb(std::string_view args, void*) {
    char buf[200] = {};
    const size_t n = args.size() < sizeof buf - 1 ? args.size() : sizeof buf - 1;
    memcpy(buf, args.data(), n);
    Probe p;
    int frames = 120, raw = 0;
    if (sscanf_s(buf, "probe %f %f %f %f %f %f %d %d", &p.eye[0], &p.eye[1], &p.eye[2], &p.target[0], &p.target[1],
                 &p.target[2], &frames, &raw) >= 6) {
        p.frames = frames < 1 ? 1 : frames > 3600 ? 3600 : frames;
        p.raw = raw != 0;
        g_probe = p;
        LOG_INFO("[levels] camera lift probe: eye (%.0f, %.0f, %.0f) target (%.0f, %.0f, %.0f) for %d frames%s", p.eye[0],
                 p.eye[1], p.eye[2], p.target[0], p.target[1], p.target[2], p.frames, p.raw ? " (raw)" : "");
        return g_on;
    }
    CloseEpisode();
    LOG_INFO("[levels] camera lift: %s, %llu frames moved in %llu runs this level", g_on ? "on" : "off",
             static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_episodes));
    return true;
}

void OnStart(const LevelStart& s, void*) {
    CloseEpisode();
    if (g_frames) {
        LOG_INFO("[levels] camera lift: %llu frames in %llu runs last level", static_cast<unsigned long long>(g_frames),
                 static_cast<unsigned long long>(g_episodes));
        g_frames = g_episodes = 0;
    }
    const bool on = s.source != Source::Vanilla;
    if (on != g_on) LOG_INFO("[levels] camera lift %s (%s)", on ? "on" : "off", s.key ? s.key : "?");
    g_on = on;
    // Only a pack or Test level asks for MirageCamera's Render hook; a vanilla map never runs the correction.
    mirage::camoverride::SetAuto(on ? &Correct : nullptr);
}

class LevelCamera final : public Module {
public:
    const char* Name() const override { return "LevelCamera"; }
    const char* Description() const override {
        return "pack and Test levels: the drawn camera is lifted out of the land when the game's eye is under it (render only)";
    }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 58; }
    bool Install() override {
        config::EnsureKey(Name(), "Clearance", "30");
        g_clearance = config::GetFloat(Name(), "Clearance", 30.f);
        if (!(g_clearance >= 0.f && g_clearance <= 400.f)) g_clearance = 30.f;
        config::EnsureKey(Name(), "MinDepth", "300");
        g_minDepth = config::GetFloat(Name(), "MinDepth", 300.f);
        if (!(g_minDepth >= 0.f && g_minDepth <= 4000.f)) g_minDepth = 300.f;
        g_start = OnLevelStart(&OnStart, nullptr);
        testcmd::Register("camera.lift", &Verb);
        LOG_INFO("[levels] camera lift ready (acts on pack and Test levels, clearance %.0f, min depth %.0f world units)",
                 g_clearance, g_minDepth);
        return true;
    }
    void Uninstall() override {
        if (g_start) RemoveOnLevelStart(g_start);
        g_start = 0;
        g_on = false;
        mirage::camoverride::SetAuto(nullptr);
    }
};
}  // namespace

MELANGE_MODULE(LevelCamera);
}  // namespace melange::levels::camlift
