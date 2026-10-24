// Offline self-test for MirageCamera's logic (render::SetCameraOverride): the look-at matrices against the engine's
// XCamera convention, the field-of-view projection, validation and the override's lifetime. Exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <string>

#include "render/mirage/camoverride_logic.h"

namespace {
namespace lg = melange::mirage::camoverride::logic;
int g_pass = 0, g_fail = 0;
void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  FAIL: %s\n", what.c_str());
    }
}
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void Mul(const float m[16], const float p[3], float out[3]) {  // point, glLoadMatrixf layout
    for (int r = 0; r < 3; ++r) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}

lg::Spec Spec(float px, float py, float pz, float tx, float ty, float tz, float ux = 0, float uy = 1, float uz = 0) {
    lg::Spec s{};
    s.pos[0] = px, s.pos[1] = py, s.pos[2] = pz;
    s.target[0] = tx, s.target[1] = ty, s.target[2] = tz;
    s.up[0] = ux, s.up[1] = uy, s.up[2] = uz;
    return s;
}

void TestLookAt() {
    const lg::Spec s = Spec(100, 250, 400, 120, 40, -30, 0, 1, 0);
    float v[16], inv[16];
    lg::LookAt(s, v, inv);
    float e[3];
    Mul(v, s.pos, e);
    Expect(Near(e[0], 0) && Near(e[1], 0) && Near(e[2], 0, 1e-3f), "the eye maps to the origin");
    float t[3];
    Mul(v, s.target, t);
    const float d = std::sqrt(20.f * 20 + 210.f * 210 + 430.f * 430);
    Expect(Near(t[0], 0, 1e-3f) && Near(t[1], 0, 1e-3f) && Near(t[2], -d, 1e-2f), "the target lies on -Z at its distance");
    const float above[3] = {100, 251, 400};
    float a[3];
    Mul(v, above, a);
    Expect(a[1] > 0.8f, "world up stays up on screen");
    const float right[3] = {101, 250, 400};  // looking toward -Z, +X is to the right
    float r[3];
    Mul(v, right, r);
    Expect(r[0] > 0.f, "right-handed: +X is right when looking down -Z");
    // inv * v = identity
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += inv[k * 4 + row] * v[c * 4 + k];
            if (!Near(sum, c == row ? 1.f : 0.f, 1e-4f)) {
                Expect(false, "inverse view * view = I");
                return;
            }
        }
    Expect(true, "inverse view * view = I");
    Expect(Near(inv[12], 100) && Near(inv[13], 250) && Near(inv[14], 400), "inverse view translation is the eye (render::Camera.pos)");
    // render::Camera.fwd = -invView[8..10] points at the target.
    const float fx = -inv[8], fy = -inv[9], fz = -inv[10];
    Expect(Near(fx, 20 / d, 1e-4f) && Near(fy, -210 / d, 1e-4f) && Near(fz, -430 / d, 1e-4f), "fwd points at the target");
    // The basis is orthonormal.
    const float* R = v;
    float m = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float dot = R[i] * R[j] + R[4 + i] * R[4 + j] + R[8 + i] * R[8 + j];
            m = std::fmax(m, std::fabs(dot - (i == j ? 1.f : 0.f)));
        }
    Expect(m < 1e-5f, "orthonormal rotation");
}

void TestDegenerate() {
    float v[16], inv[16];
    lg::LookAt(Spec(0, 500, 0, 0, 0, 0, 0, 1, 0), v, inv);  // straight down with up = +Y
    bool finite = true;
    for (float x : v) finite = finite && std::isfinite(x);
    Expect(finite, "looking along up still gives a finite matrix (the engine's +Y/+Z fallback)");
    float t[3];
    const float target[3] = {0, 0, 0};
    Mul(v, target, t);
    Expect(Near(t[2], -500, 1e-2f), "and still looks at the target");
    lg::LookAt(Spec(0, 0, 10, 0, 0, 0, 0, 0, 0), v, inv);  // zero up
    finite = true;
    for (float x : v) finite = finite && std::isfinite(x);
    Expect(finite, "a zero up vector falls back");
}

void TestFov() {
    float proj[16] = {};
    const float f60 = 1.f / std::tan(30.f * 3.14159265f / 180.f);
    proj[5] = f60;
    proj[0] = f60 * 9.f / 16.f;
    proj[10] = -1.002f;
    proj[11] = -1.f;
    proj[14] = -2.002f;
    Expect(Near(lg::FovOf(proj), 60.f, 1e-3f), "FovOf reads 60 degrees");
    float out[16];
    const float k = lg::WithFov(proj, 30.f, out);
    Expect(Near(lg::FovOf(out), 30.f, 1e-3f), "WithFov sets 30 degrees");
    Expect(Near(out[0] / out[5], proj[0] / proj[5], 1e-6f), "the aspect ratio is kept");
    Expect(out[10] == proj[10] && out[14] == proj[14] && out[11] == -1.f, "the depth range is kept");
    Expect(Near(k, std::tan(15.f * 3.14159265f / 180.f) / std::tan(30.f * 3.14159265f / 180.f), 1e-5f),
           "tangent extents scale by tan(new/2) / tan(old/2)");
    const float k0 = lg::WithFov(proj, 0.f, out);
    Expect(k0 == 1.f && out[5] == proj[5] && out[0] == proj[0], "fov 0 keeps the game's projection");
    float ortho[16] = {};
    ortho[0] = ortho[5] = 0.01f;
    ortho[15] = 1;
    Expect(lg::FovOf(ortho) == 0.f, "orthographic: no fov");
}

void TestValidate() {
    Expect(lg::Validate(Spec(0, 1, 0, 0, 0, 0)) == nullptr, "a valid spec");
    Expect(lg::Validate(Spec(1, 1, 1, 1, 1, 1)) != nullptr, "pos == target");
    Expect(lg::Validate(Spec(0, NAN, 0, 0, 0, 0)) != nullptr, "NaN pos");
    Expect(lg::Validate(Spec(0, 1, 0, 0, INFINITY, 0)) != nullptr, "infinite target");
    Expect(lg::Validate(Spec(2e6f, 1, 0, 0, 0, 0)) != nullptr, "far out of the world");
    lg::Spec s = Spec(0, 1, 0, 0, 0, 0);
    s.fovDeg = 4.9f;
    Expect(lg::Validate(s) != nullptr, "fov below 5");
    s.fovDeg = 150.f;
    Expect(lg::Validate(s) == nullptr, "fov 150");
    s.fovDeg = 151.f;
    Expect(lg::Validate(s) != nullptr, "fov above 150");
}

void TestLifetime() {
    lg::State st;
    int a = 0, b = 0;
    const lg::Spec s = Spec(0, 1, 0, 0, 0, 0);
    Expect(lg::Set(st, &a, s, 0, 100, 7) == lg::SetResult::Ok && st.active && st.expiresAfter == 130, "default 30 frames");
    Expect(lg::Set(st, &b, s, 5, 101, 7) == lg::SetResult::Busy, "another owner is busy");
    Expect(lg::Set(st, &a, Spec(1, 1, 1, 1, 1, 1), 5, 101, 7) == lg::SetResult::Invalid && st.expiresAfter == 130,
           "an invalid refresh changes nothing");
    Expect(lg::Set(st, &a, s, 1000, 110, 7) == lg::SetResult::Ok && st.expiresAfter == 710, "frames clamp to 600");
    Expect(lg::Check(st, 710, true, 7) == lg::EndReason::None && st.active, "alive through its last frame");
    Expect(lg::Check(st, 711, true, 7) == lg::EndReason::Expired && !st.active, "expires when not refreshed");
    lg::Set(st, &a, s, 10, 200, 7);
    Expect(lg::Check(st, 201, false, 7) == lg::EndReason::MatchEnded && !st.active, "ends with the match");
    lg::Set(st, &a, s, 10, 200, 7);
    Expect(lg::Check(st, 201, true, 8) == lg::EndReason::MatchEnded && !st.active, "ends when a new match VM appears");
    lg::Set(st, &a, s, 10, 200, 7);
    Expect(!lg::Clear(st, &b) && st.active, "another owner cannot clear it");
    Expect(lg::Clear(st, &a) && !st.active, "the owner clears it");
    Expect(!lg::Clear(st, &a), "clearing twice reports false");
    lg::Set(st, &b, s, 10, 200, 7);
    Expect(lg::Clear(st, nullptr) && !st.active, "a null owner clears any");
    Expect(lg::Set(st, &a, s, 3, 300, 7) == lg::SetResult::Ok, "free again after a clear");
}
}  // namespace

int main() {
    TestLookAt();
    TestDegenerate();
    TestFov();
    TestValidate();
    TestLifetime();
    printf("camoverride_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
