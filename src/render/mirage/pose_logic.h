#pragma once
// MiragePose logic without GL: the worm skeleton (W4.Worm, Bundl474.xom) and the palette maths behind
// wum.render.wormPose / wormCopy. Header-only so the offline self-test (tests/pose_selftest.cpp) checks it directly.
//
// The game skins worms on the GPU (CG/FixedFunction.cg, FFVertex*Skinned): world = Σ w · (v · BlendMat[i]) with the
// Cg row-vector convention, `float4x4 BlendMat[35] : register(c8)`, uploaded as ARB vertex program locals, four
// registers (rows) per bone. Row 3 holds the translation. BlendMat[i] = inverse bind pose (XBone.PoseMatrix) x the
// bone's world transform this frame, so for a bind-space point p, p · BlendMat[i] is where bone i puts it.
//
// A pose is a world-space delta D per bone applied after the game's matrix: BlendMat'[i] = BlendMat[i] · D_i (row
// vectors: first the game's skinning, then our move). Only draws change; the simulation never sees any of it.
#include <cmath>
#include <cstring>

namespace melange::mirage::pose::logic {
constexpr int kBones = 34;
constexpr int kFloats = kBones * 16;  // the palette as uploaded: 34 bones x 4 registers x 4 floats

// Bind-space pivots of the 34 palette bones (inverse(PoseMatrix) applied to the origin), from Bundl474.xom's XBone
// objects in the order every XSkinShape of W4.Worm lists them (xomtool inspect, 2026-11-18).
inline constexpr float kPivot[kBones][3] = {
    {0.000f, -0.827f, 0.329f},    {0.000f, -2.692f, 0.411f},    {0.000f, -7.644f, 0.375f},    {0.000f, -12.755f, 0.207f},
    {0.000f, 0.968f, 0.169f},     {-5.380f, 4.841f, 3.084f},    {5.380f, 4.841f, 3.084f},     {4.611f, 3.395f, 2.494f},
    {-4.611f, 3.395f, 2.494f},    {0.000f, 3.426f, 4.429f},     {5.647f, 3.713f, 1.032f},     {-5.647f, 3.713f, 1.032f},
    {1.865f, 15.356f, 5.196f},    {2.042f, 17.252f, 3.406f},    {-1.865f, 15.356f, 5.196f},   {-2.042f, 17.252f, 3.406f},
    {0.000f, 0.968f, 0.169f},     {9.547f, 0.646f, -2.053f},    {13.621f, 1.663f, -4.892f},   {14.910f, 0.790f, -6.449f},
    {15.045f, 1.748f, -2.927f},   {17.034f, 1.215f, -3.718f},   {14.970f, 1.158f, -0.567f},   {16.936f, 0.104f, -0.144f},
    {12.633f, 0.299f, 0.550f},    {0.000f, 0.968f, 0.169f},     {-9.547f, 0.646f, -2.053f},   {-13.621f, 1.663f, -4.892f},
    {-14.910f, 0.790f, -6.449f},  {-15.045f, 1.748f, -2.927f},  {-17.034f, 1.215f, -3.718f},  {-14.970f, 1.158f, -0.567f},
    {-16.937f, 0.104f, -0.144f},  {-12.633f, 0.299f, 0.550f},
};
inline constexpr const char* kBoneNames[kBones] = {
    "main",          "tail",           "tail2",          "tail3",           "head",           "upperlip_right",
    "upperlip_left", "bottomlip_left", "bottomlip_right", "bottomlip",      "mouthmiddle_left", "mouthmiddle_right",
    "eyetop_left",   "eyebrow_left",   "eyetop_right",   "eyebrow_right",   "shoulder_left",  "wrist_left",
    "pinkyknuckle_left", "pinky_left", "indexknuckle_left", "index_left",   "foreknuckle_left", "fore_left",
    "thumb_left",    "shoulder_right", "wrist_right",    "pinkyknuckle_right", "pinky_right", "indexknuckle_right",
    "index_right",   "foreknuckle_right", "fore_right",  "thumb_right",
};
// The neck: the head subtree's skin weight is 0 below bind y = -6 and >= 0.94 above y = 2, so a cut at y = -2 is the
// middle of the blend band.
inline constexpr float kNeck[3] = {0.f, -2.f, 0.2f};

enum Group : int { kHead = 0, kTail = 1, kLeftHand = 2, kRightHand = 3, kGroups = 4 };
inline constexpr const char* kGroupNames[kGroups] = {"head", "tail", "leftHand", "rightHand"};
inline int GroupOf(int bone) {
    if (bone >= 4 && bone <= 15) return kHead;
    if (bone >= 1 && bone <= 3) return kTail;
    if (bone >= 16 && bone <= 24) return kLeftHand;
    if (bone >= 25 && bone <= 33) return kRightHand;
    return -1;
}
// The bone whose pivot a group turns about by default (head, tail, wrist_left, wrist_right).
inline constexpr int kGroupPivotBone[kGroups] = {4, 1, 17, 26};

// ---------------------------------------------------------------- row-vector 4x4 (m[r*4+c], v' = v * M)
struct M4 {
    float m[16];
};
inline M4 Identity() {
    M4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
}
inline M4 Mul(const M4& a, const M4& b) {  // apply a, then b
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
inline M4 Translate(const float t[3]) {
    M4 r = Identity();
    r.m[12] = t[0];
    r.m[13] = t[1];
    r.m[14] = t[2];
    return r;
}
inline M4 Translate(float x, float y, float z) {
    const float t[3] = {x, y, z};
    return Translate(t);
}
// Rotation by Euler angles in degrees: roll about Z, then pitch about X, then yaw about Y (world axes). Yaw turns +Z
// toward +X like a worm's facing (sin yaw, 0, cos yaw); positive pitch tips +Z up toward +Y.
inline M4 Rotate(float pitchDeg, float yawDeg, float rollDeg) {
    constexpr float k = 3.14159265358979f / 180.f;
    const float p = pitchDeg * k, y = yawDeg * k, r = rollDeg * k;
    M4 z = Identity(), x = Identity(), w = Identity();
    // row-vector forms (v * M)
    z.m[0] = std::cos(r), z.m[1] = std::sin(r), z.m[4] = -std::sin(r), z.m[5] = std::cos(r);
    x.m[5] = std::cos(p), x.m[6] = -std::sin(p), x.m[9] = std::sin(p), x.m[10] = std::cos(p);
    w.m[0] = std::cos(y), w.m[2] = -std::sin(y), w.m[8] = std::sin(y), w.m[10] = std::cos(y);
    return Mul(Mul(z, x), w);
}
// Turn about a world pivot, then move.
inline M4 About(const float pivot[3], const M4& rot, const float move[3]) {
    const float neg[3] = {-pivot[0], -pivot[1], -pivot[2]};
    const float back[3] = {pivot[0] + move[0], pivot[1] + move[1], pivot[2] + move[2]};
    return Mul(Mul(Translate(neg), rot), Translate(back));
}
// Scale along the unit axis `u` by `s` (the other two directions kept), about a world point.
inline M4 FlattenAbout(const float pivot[3], const float u[3], float s) {
    M4 f = Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) f.m[i * 4 + j] -= (1.f - s) * u[i] * u[j];
    const float neg[3] = {-pivot[0], -pivot[1], -pivot[2]};
    return Mul(Mul(Translate(neg), f), Translate(pivot));
}
inline M4 Uniform(const float pivot[3], float s) {
    M4 f = Identity();
    f.m[0] = f.m[5] = f.m[10] = s;
    const float neg[3] = {-pivot[0], -pivot[1], -pivot[2]};
    return Mul(Mul(Translate(neg), f), Translate(pivot));
}
inline void Point(const M4& m, const float p[3], float out[3]) {
    for (int j = 0; j < 3; ++j) out[j] = p[0] * m.m[j] + p[1] * m.m[4 + j] + p[2] * m.m[8 + j] + m.m[12 + j];
}
inline void Dir(const M4& m, const float d[3], float out[3]) {
    for (int j = 0; j < 3; ++j) out[j] = d[0] * m.m[j] + d[1] * m.m[4 + j] + d[2] * m.m[8 + j];
}
inline bool Normalize(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(l > 1e-6f)) return false;
    v[0] /= l, v[1] /= l, v[2] /= l;
    return true;
}
inline M4 BoneOf(const float* palette, int bone) {
    M4 r;
    std::memcpy(r.m, palette + bone * 16, sizeof r.m);
    return r;
}
// The world position bone `bone` puts its bind pivot at.
inline void PivotWorld(const float* palette, int bone, float out[3]) { Point(BoneOf(palette, bone), kPivot[bone], out); }

// ---------------------------------------------------------------- the pose
struct Edit {
    bool on = false;
    bool hasPivot = false;
    float pivot[3] = {0, 0, 0};
    float move[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};       // pitch, yaw, roll in degrees
    bool hasTarget = false;         // hands: put the wrist pivot here (world) instead of moving by `move`
    float target[3] = {0, 0, 0};
};
struct Spec {
    Edit root;                       // the whole worm (default pivot: the main bone's)
    Edit group[kGroups];             // head, tail, leftHand, rightHand (default pivot: kGroupPivotBone)
    bool stump = false;              // flatten the head subtree onto the neck plane
};

inline M4 EditDelta(const Edit& e, const float defaultPivot[3]) {
    const float* p = e.hasPivot ? e.pivot : defaultPivot;
    const M4 r = Rotate(e.rot[0], e.rot[1], e.rot[2]);
    if (!e.hasTarget) return About(p, r, e.move);
    const float move[3] = {e.target[0] - p[0], e.target[1] - p[1], e.target[2] - p[2]};
    return About(p, r, move);
}

// The neck point and the worm's "up" in world space after `delta` (the neck frame the stump flattens across).
inline void NeckFrame(const float* palette, const M4& delta, float neck[3], float up[3]) {
    const M4 m = Mul(BoneOf(palette, 4), delta);
    Point(m, kNeck, neck);
    const float y[3] = {0, 1, 0};
    Dir(m, y, up);
    if (!Normalize(up)) up[0] = 0, up[1] = 1, up[2] = 0;
}

// in: the game's palette; out: the posed one. Deltas: the root first, then the group's own edit (about its pivot
// after the root moved it), then the stump on the head subtree.
inline void Compose(const float* in, const Spec& s, float* out) {
    float rootPivot[3];
    PivotWorld(in, 0, rootPivot);
    const M4 root = s.root.on ? EditDelta(s.root, rootPivot) : Identity();
    M4 groupDelta[kGroups];
    for (int g = 0; g < kGroups; ++g) {
        if (!s.group[g].on) {
            groupDelta[g] = root;
            continue;
        }
        float p[3];
        Point(Mul(BoneOf(in, kGroupPivotBone[g]), root), kPivot[kGroupPivotBone[g]], p);
        groupDelta[g] = Mul(root, EditDelta(s.group[g], p));
    }
    M4 stump = Identity();
    if (s.stump) {
        float neck[3], up[3];
        NeckFrame(in, groupDelta[kHead], neck, up);
        stump = FlattenAbout(neck, up, 0.02f);
    }
    for (int b = 0; b < kBones; ++b) {
        const int g = GroupOf(b);
        M4 d = g < 0 ? root : groupDelta[g];
        if (g == kHead && s.stump) d = Mul(d, stump);
        const M4 r = Mul(BoneOf(in, b), d);
        std::memcpy(out + b * 16, r.m, sizeof r.m);
    }
}

// The severed head: the head subtree carried by `head` (a world transform applied after moving the original neck to
// the origin, so head = Rotate(..) * Translate(where)), and every other bone collapsed into the neck so the blend band
// closes over the cut. `in` is the game's palette.
inline void ComposeCopy(const float* in, const M4& head, float* out) {
    float neck[3], up[3];
    NeckFrame(in, Identity(), neck, up);
    const float neg[3] = {-neck[0], -neck[1], -neck[2]};
    const M4 carry = Mul(Translate(neg), head);
    const M4 collapse = Mul(Uniform(neck, 0.f), carry);
    for (int b = 0; b < kBones; ++b) {
        const M4 r = Mul(BoneOf(in, b), GroupOf(b) == kHead ? carry : collapse);
        std::memcpy(out + b * 16, r.m, sizeof r.m);
    }
}

// Which worm a skinned draw is: the main bone's pivot against each worm's feet plus the centre offset. Returns the
// index into `feet` of the nearest within `maxDist`, -1 otherwise.
constexpr float kCentreY = 19.f;   // the main pivot sits about this far above the feet (bind tail tip y = -12.8)
inline int Identify(const float* palette, const float (*feet)[3], int n, float maxDist) {
    float p[3];
    PivotWorld(palette, 0, p);
    int best = -1;
    float bestD = maxDist * maxDist;
    for (int i = 0; i < n; ++i) {
        const float d[3] = {p[0] - feet[i][0], p[1] - (feet[i][1] + kCentreY), p[2] - feet[i][2]};
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (dd < bestD) bestD = dd, best = i;
    }
    return best;
}
}  // namespace melange::mirage::pose::logic
