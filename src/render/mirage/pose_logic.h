#pragma once
// MiragePose logic without GL: the worm skeleton (W4.Worm, Bundl474.xom) and the palette maths behind
// wum.render.wormPose / wormCopy. Header-only so the offline self-test (tests/pose_selftest.cpp) checks it directly.
//
// The palette is the same on both skinning routes. Build #1077 blends worms on the CPU from XBone+0x70 (the 34 matrices
// of each XSkinShape, docs/gravespade-re-pose-cpu-skin.md); a build that skins on the GPU uses the Cg programs
// (CG/FixedFunction.cg, FFVertex*Skinned): world = Σ w · (v · BlendMat[i]) with the
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
// Rigid attachments on the head (hat, moustache, glasses: separate unskinned draws placed by their `model` matrix).
enum class Attach : unsigned char { Keep = 0, Hide = 1, Follow = 2 };
struct Spec {
    Edit root;                       // the whole worm (default pivot: the main bone's)
    Edit group[kGroups];             // head, tail, leftHand, rightHand (default pivot: kGroupPivotBone)
    bool stump = false;              // flatten the head subtree onto the neck plane
    Attach attach = Attach::Keep;    // hide.attachments: "hide" or "follow" (follow is hide under a stump)
    // No edit, no stump, no attachment rule: the pose only tracks the bones (wormBones) and changes nothing drawn.
    bool Empty() const {
        if (root.on || stump || attach != Attach::Keep) return false;
        for (const Edit& g : group)
            if (g.on) return false;
        return true;
    }
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
// after the root moved it), then the stump on the head subtree. headDelta (optional): the head group's world delta
// without the stump, which head attachments that follow the pose get.
inline void Compose(const float* in, const Spec& s, float* out, M4* headDelta = nullptr) {
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
    if (headDelta) *headDelta = groupDelta[kHead];
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
constexpr float kCentreY = 19.f;   // the first guess: the main pivot sits about this far above the feet
inline int Identify(const float* palette, const float (*feet)[3], int n, float maxDist, float centreY = kCentreY) {
    float p[3];
    PivotWorld(palette, 0, p);
    int best = -1;
    float bestD = maxDist * maxDist;
    for (int i = 0; i < n; ++i) {
        const float d[3] = {p[0] - feet[i][0], p[1] - (feet[i][1] + centreY), p[2] - feet[i][2]};
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (dd < bestD) bestD = dd, best = i;
    }
    return best;
}

// The centre offset learned in game: a skinned draw whose main pivot is horizontally on one worm (within kSameXZ)
// while every other worm is far (kOtherXZ) is unambiguous whatever the height, so its height over the feet is a
// sample. kCentreY is only the first guess; after a few samples Identify uses the running mean, so two worms close
// together (the mount) are told apart by a measured offset, not a guess.
struct Centre {
    static constexpr float kSameXZ = 8.f, kOtherXZ = 40.f, kMinY = 0.f, kMaxY = 40.f;
    static constexpr int kWindow = 32;
    float y = kCentreY;
    int samples = 0;
    // Returns true when the draw was a sample.
    bool Sample(const float p[3], const float (*feet)[3], int n) {
        int hit = -1;
        for (int i = 0; i < n; ++i) {
            const float dx = p[0] - feet[i][0], dz = p[2] - feet[i][2];
            const float h = dx * dx + dz * dz;
            if (h <= kSameXZ * kSameXZ) {
                if (hit >= 0) return false;  // two worms under the pivot
                hit = i;
            } else if (h < kOtherXZ * kOtherXZ) {
                return false;  // another worm close by: ambiguous
            }
        }
        if (hit < 0) return false;
        const float dy = p[1] - feet[hit][1];
        if (!(dy >= kMinY && dy <= kMaxY)) return false;
        ++samples;
        const int w = samples < kWindow ? samples : kWindow;
        y += (dy - y) / static_cast<float>(w);
        return true;
    }
};

// Rigid head attachments: the attachment draw's origin (its model translation) within `radius` of one of the
// worm's unposed head bone pivots (bones 4..15) belongs to that head.
constexpr int kHeadFirst = 4, kHeadBones = 12;
constexpr float kAttachRadius = 9.f;
inline bool NearHead(const float t[3], const float (*head)[3], float radius = kAttachRadius) {
    for (int i = 0; i < kHeadBones; ++i) {
        const float d[3] = {t[0] - head[i][0], t[1] - head[i][1], t[2] - head[i][2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= radius * radius) return true;
    }
    return false;
}

// ---------------------------------------------------------------- the CPU-skinned back end (build #1077)
// docs/gravespade-re-pose-anim.md and gravespade-re-pose-cpu-skin.md. Three render-only layers, each at its own point of
// the engine's frame:
//   Body  - the worm's XMeshInstance transform, written after WXWormGraphicEntity's update (0x5a4740). Hats, glasses,
//           the locators and the shadow follow it.
//   Clips - the XAnimInstance {weight, time, rate} table, patched around Evaluate (0x7ad0ef) and put back.
//   Spec  - the palette above (bone+0x70, the CPU equivalent of BlendMat), patched around XOglDrawAction's XSkinShape
//           method (0x790cd0). It stacks on the moved and animated body, because the cull pass rebuilds bone+0x70 from
//           the joints every frame before the draw.

// The engine draws a worm's mesh this far above its logic position (0x5a00b0: container.pos + (0, 3, 0)).
constexpr float kDrawLift = 3.f;
// On the CPU path the main bone's pivot sits at the mesh origin, kDrawLift over the logic feet ([L], instance C).
constexpr float kCpuCentreY = kDrawLift;

struct Body {
    bool on = false;
    bool turn = false;          // a rotation is written (pitch, roll, yaw or heading given)
    float pitch = 0;            // degrees, in the worm's frame after the heading: + tips it forward onto its face
    float roll = 0;             // degrees about its forward axis (combined with pitch the order is inferred: Ry*Rx*Rz)
    float yaw = 0;              // degrees: a delta on the drawn yaw, or the absolute heading with absHeading
    bool absHeading = false;
    float move[3] = {0, 0, 0};  // a delta on the drawn origin, or (absAt) the logic feet position to draw at
    bool absAt = false;
};

// The Euler angles (radians) for XMeshInstance::SetRotation: {x = pitch, y = heading, z = roll}.
inline void BodyEuler(const Body& b, float drawnYawRad, float out[3]) {
    constexpr float k = 3.14159265358979f / 180.f;
    out[0] = b.pitch * k;
    out[1] = b.absHeading ? b.yaw * k : drawnYawRad + b.yaw * k;
    out[2] = b.roll * k;
}
// The mesh origin to draw at, from the origin the game just wrote.
inline void BodyOrigin(const Body& b, const float drawn[3], float out[3]) {
    if (b.absAt) {
        out[0] = b.move[0], out[1] = b.move[1] + kDrawLift, out[2] = b.move[2];
        return;
    }
    for (int i = 0; i < 3; ++i) out[i] = drawn[i] + b.move[i];
}
// The same turn as a row-vector matrix in this file's convention, for checks and for predicting a pose: the engine's
// +pitch takes the worm's up (+Y) toward its facing, which is Rotate's -pitch; the heading is Rotate's yaw.
inline M4 BodyRotate(float pitchDeg, float headingDeg, float rollDeg) { return Rotate(-pitchDeg, headingDeg, rollDeg); }
// Where a point given relative to the unturned mesh origin (bind space, the worm facing +Z) is drawn.
inline void BodyPoint(const float origin[3], float pitchDeg, float headingDeg, float rollDeg, const float local[3],
                      float out[3]) {
    Point(Mul(BodyRotate(pitchDeg, headingDeg, rollDeg), Translate(origin)), local, out);
}

// Clip edits, by clip id in the worm's XAnimClipLibrary (resolved from names when the pose is set).
struct Clips {
    static constexpr int kMax = 8, kSliders = 8;
    int n = 0;
    short id[kMax] = {};
    float time[kMax] = {}, weight[kMax] = {};
    bool base = false;  // clipMode "base": every clip but Base (baseId) and ours at weight 0
    short baseId = -1;
    int sn = 0;         // pose sliders (HeadRotX, RightArmRotY, TailAngle...): t in 0..2, 1 = neutral
    short sid[kSliders] = {};
    float st[kSliders] = {};
    bool On() const { return n > 0 || sn > 0 || base; }
};
constexpr float kMaxClipWeight = 5.f;  // above about 5 some scale and translation channels exaggerate ([L])
inline float ClampWeight(float w) { return !(w > 0.f) ? 0.f : (w > kMaxClipWeight ? kMaxClipWeight : w); }
inline float ClampSlider(float t) { return !(t > 0.f) ? 0.f : (t > 2.f ? 2.f : t); }
inline float ClampTime(float t) { return !(t > 0.f) ? 0.f : (t > 600.f ? 600.f : t); }

// Applies the edits to an XAnimInstance entry table (count entries of {weight, time, rate}). Our clips are held (rate
// 0); a slider keeps its own weight (1 when the game had it off) and takes our time.
inline void ApplyClips(const Clips& c, float* e, int count) {
    if (c.base)
        for (int i = 0; i < count; ++i)
            if (i != c.baseId) e[i * 3] = 0.f;
    for (int k = 0; k < c.n; ++k) {
        const int i = c.id[k];
        if (i < 0 || i >= count) continue;
        e[i * 3 + 0] = c.weight[k];
        e[i * 3 + 1] = c.time[k];
        e[i * 3 + 2] = 0.f;
    }
    for (int k = 0; k < c.sn; ++k) {
        const int i = c.sid[k];
        if (i < 0 || i >= count) continue;
        if (!(e[i * 3] > 0.f)) e[i * 3] = 1.f;
        e[i * 3 + 1] = c.st[k];
        e[i * 3 + 2] = 0.f;
    }
}

// Which worm a CPU-skinned draw is. Each worm has its own XBone objects ([L]), so the first bone's address is a key
// that survives any pose. It is learned on a draw that is unambiguous by position (the main pivot within kLearn of one
// worm's expected centre and every other worm kApart or more away), and then trusted while the pivot stays within
// kTrust of that worm's centre, however close another worm is (the mount: the attacker drawn 8 units over the victim).
struct SkinKeys {
    static constexpr int kN = 32;
    static constexpr float kLearn = 8.f, kApart = 20.f, kTrust = 26.f;
    struct E {
        unsigned key = 0;
        int slot = -1;
        unsigned long long seen = 0;
    } e[kN];
    int Find(unsigned key) const {
        for (int i = 0; i < kN; ++i)
            if (e[i].key == key && e[i].slot >= 0) return i;
        return -1;
    }
    void Forget(int slot) {
        for (E& x : e)
            if (x.slot == slot) x = E{};
    }
    void Learn(unsigned key, int slot, unsigned long long frame) {
        int i = Find(key);
        if (i < 0 || e[i].slot != slot) {
            if (i >= 0) e[i] = E{};
            Forget(slot);  // a slot has one worm: an older key for it is stale
            i = 0;
            for (int k = 0; k < kN; ++k) {
                if (e[k].slot < 0) {
                    i = k;
                    break;
                }
                if (e[k].seen < e[i].seen) i = k;
            }
        }
        e[i].key = key, e[i].slot = slot, e[i].seen = frame;
    }
    // centres[i]: worm i's expected main pivot this frame; slots[i]: its slot. Returns the index into centres, or -1.
    int Resolve(unsigned key, const float p[3], const float (*centres)[3], const int* slots, int n, unsigned long long frame) {
        auto d2 = [&](int i) {
            const float d[3] = {p[0] - centres[i][0], p[1] - centres[i][1], p[2] - centres[i][2]};
            return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        };
        const int k = Find(key);
        if (k >= 0)
            for (int i = 0; i < n; ++i)
                if (slots[i] == e[k].slot && d2(i) <= kTrust * kTrust) {
                    e[k].seen = frame;
                    return i;
                }
        int best = -1;
        float b1 = 1e30f, b2 = 1e30f;
        for (int i = 0; i < n; ++i) {
            const float d = d2(i);
            if (d < b1) {
                b2 = b1, b1 = d, best = i;
            } else if (d < b2) {
                b2 = d;
            }
        }
        if (best < 0 || b1 > kTrust * kTrust) return -1;
        if (b1 <= kLearn * kLearn && b2 >= kApart * kApart) Learn(key, slots[best], frame);
        return best;
    }
};
}  // namespace melange::mirage::pose::logic
