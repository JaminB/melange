#pragma once
// render::SetCameraOverride without the game: the matrices an override draws with and the rules that end it. Header
// only, so camoverride_selftest checks it offline.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace melange::mirage::camoverride::logic {
// Matrices are float[16] in glLoadMatrixf layout (translation in [12..14]), as render::Camera holds them.
struct Spec {
    float pos[3], target[3], up[3];
    float fovDeg;  // vertical field of view; 0 = keep the game's
};

constexpr float kMinFovDeg = 5.f, kMaxFovDeg = 150.f;
constexpr int kDefaultFrames = 30, kMaxFrames = 600;
constexpr float kMinDistance = 1e-3f;  // pos and target closer than this: refused
constexpr float kMaxCoord = 1e6f;

inline bool Finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
inline bool InRange3(const float v[3]) {
    return Finite3(v) && std::fabs(v[0]) <= kMaxCoord && std::fabs(v[1]) <= kMaxCoord && std::fabs(v[2]) <= kMaxCoord;
}

// Null when the spec can be drawn, else why not (for the Lua error).
inline const char* Validate(const Spec& s) {
    if (!InRange3(s.pos)) return "pos must be finite and within 1e6";
    if (!InRange3(s.target)) return "target must be finite and within 1e6";
    if (!Finite3(s.up)) return "up must be finite";
    const float d[3] = {s.pos[0] - s.target[0], s.pos[1] - s.target[1], s.pos[2] - s.target[2]};
    if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) < kMinDistance) return "pos and target are the same point";
    if (s.fovDeg != 0.f && !(s.fovDeg >= kMinFovDeg && s.fovDeg <= kMaxFovDeg)) return "fov must be from 5 to 150 degrees";
    return nullptr;
}

inline void Normalize(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}
inline void Cross(const float a[3], const float b[3], float out[3]) {
    const float r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(out, r, sizeof r);
}
inline float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The engine's XCamera basis (0x6e1d6c): back = pos - target, right = up x back, with the same fallbacks when `up` is
// parallel to the view direction (world +Y, then +Z), then up' = back x right.
inline void Basis(const Spec& s, float right[3], float up[3], float back[3]) {
    for (int i = 0; i < 3; ++i) back[i] = s.pos[i] - s.target[i];
    Normalize(back);
    float u[3] = {s.up[0], s.up[1], s.up[2]};
    Normalize(u);
    Cross(u, back, right);
    if (Dot(right, right) < 1e-6f) {
        const float y[3] = {0, 1, 0};
        Cross(y, back, right);
        if (Dot(right, right) < 1e-6f) {
            const float z[3] = {0, 0, 1};
            Cross(z, back, right);
        }
    }
    Normalize(right);
    Cross(back, right, up);
    Normalize(up);
}

// gluLookAt: world -> eye, and its inverse (eye -> world).
inline void LookAt(const Spec& s, float view[16], float invView[16]) {
    float r[3], u[3], b[3];
    Basis(s, r, u, b);
    const float m[16] = {r[0], u[0], b[0], 0, r[1], u[1], b[1], 0, r[2], u[2], b[2], 0,
                         -Dot(r, s.pos), -Dot(u, s.pos), -Dot(b, s.pos), 1};
    memcpy(view, m, sizeof m);
    const float inv[16] = {r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, b[0], b[1], b[2], 0, s.pos[0], s.pos[1], s.pos[2], 1};
    memcpy(invView, inv, sizeof inv);
}

// The game's perspective projection with a new vertical field of view: the aspect ratio, any off-centre terms and the
// depth range stay the game's. Returns the factor the frustum's tangent extents (XCamera +0x104..+0x110) scale by.
inline float WithFov(const float proj[16], float fovDeg, float out[16]) {
    memcpy(out, proj, 16 * sizeof(float));
    if (fovDeg <= 0.f || proj[5] == 0.f || proj[0] == 0.f) return 1.f;
    const float f = 1.f / std::tan(fovDeg * 3.14159265358979f / 360.f);
    const float k = proj[5] / f;  // old y scale / new y scale
    out[0] = proj[0] / k;
    out[5] = f;
    return k;
}

// Vertical field of view of a perspective projection, degrees (0 for an orthographic or unreadable one).
inline float FovOf(const float proj[16]) {
    if (proj[11] == 0.f || !(proj[5] > 0.f)) return 0.f;
    return 2.f * std::atan(1.f / proj[5]) * 360.f / 6.28318530717959f;
}

// Lifetime: an override lasts `frames` frames after its last refresh, and ends at a match end or when its owner goes.
struct State {
    bool active = false;
    const void* owner = nullptr;
    Spec spec{};
    uint64_t expiresAfter = 0;  // the last frame it may draw
    uint32_t matchSerial = 0;
};

enum class SetResult : uint8_t { Ok, Busy, Invalid };

inline int ClampFrames(int frames) { return frames <= 0 ? kDefaultFrames : frames > kMaxFrames ? kMaxFrames : frames; }

inline SetResult Set(State& st, const void* owner, const Spec& s, int frames, uint64_t now, uint32_t matchSerial) {
    if (st.active && st.owner != owner) return SetResult::Busy;
    if (Validate(s)) return SetResult::Invalid;
    st.active = true;
    st.owner = owner;
    st.spec = s;
    st.expiresAfter = now + static_cast<uint64_t>(ClampFrames(frames));
    st.matchSerial = matchSerial;
    return SetResult::Ok;
}

// True if `owner` held it (any owner when owner is null).
inline bool Clear(State& st, const void* owner) {
    if (!st.active || (owner && st.owner != owner)) return false;
    st = State{};
    return true;
}

enum class EndReason : uint8_t { None, Expired, MatchEnded };
// Called before each use: ends a stale override.
inline EndReason Check(State& st, uint64_t now, bool inMatch, uint32_t matchSerial) {
    if (!st.active) return EndReason::None;
    if (!inMatch || matchSerial != st.matchSerial) {
        st = State{};
        return EndReason::MatchEnded;
    }
    if (now > st.expiresAfter) {
        st = State{};
        return EndReason::Expired;
    }
    return EndReason::None;
}
}  // namespace melange::mirage::camoverride::logic
