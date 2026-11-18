// MiragePose logic (src/render/mirage/pose_logic.h): the palette maths behind wum.render.wormPose / wormCopy.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "render/mirage/pose_logic.h"

namespace lg = melange::mirage::pose::logic;
namespace {
int g_pass = 0, g_fail = 0;
void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}
bool Near(const float a[3], const float b[3], float eps = 1e-3f) {
    return std::fabs(a[0] - b[0]) < eps && std::fabs(a[1] - b[1]) < eps && std::fabs(a[2] - b[2]) < eps;
}
std::string V(const float a[3]) {
    char b[96];
    snprintf(b, sizeof b, "(%.3f, %.3f, %.3f)", a[0], a[1], a[2]);
    return b;
}

// A palette that puts the worm, unposed, with its bind origin at `at` and facing yaw `yawDeg`.
void Palette(float* p, const float at[3], float yawDeg) {
    const lg::M4 m = lg::Mul(lg::Rotate(0, yawDeg, 0), lg::Translate(at));
    for (int b = 0; b < lg::kBones; ++b) std::memcpy(p + b * 16, m.m, sizeof m.m);
}

void World(const float* pal, int bone, const float bind[3], float out[3]) { lg::Point(lg::BoneOf(pal, bone), bind, out); }
}  // namespace

int main() {
    const float at[3] = {100, 50, -20};
    float pal[lg::kFloats], out[lg::kFloats];
    Palette(pal, at, 0);

    {  // maths
        const float z[3] = {0, 0, 1};
        float r[3];
        lg::Dir(lg::Rotate(0, 90, 0), z, r);
        const float e1[3] = {1, 0, 0};
        Expect(Near(r, e1), "yaw 90 turns +Z to +X (the worm's facing (sin, 0, cos)): " + V(r));
        lg::Dir(lg::Rotate(90, 0, 0), z, r);
        const float e2[3] = {0, 1, 0};
        Expect(Near(r, e2), "pitch 90 tips +Z up: " + V(r));
        const float pv[3] = {1, 2, 3}, mv[3] = {0, 0, 0};
        float q[3];
        lg::Point(lg::About(pv, lg::Rotate(0, 180, 0), mv), pv, q);
        Expect(Near(q, pv), "a turn about a pivot keeps the pivot");
    }
    {  // no edits: the palette is unchanged
        lg::Spec s;
        lg::Compose(pal, s, out);
        Expect(std::memcmp(pal, out, sizeof pal) == 0, "an empty pose leaves the palette as the game sent it");
    }
    {  // prone: pitch -90 about the main pivot lays the worm on its back (up becomes -Z... toward its back)
        lg::Spec s;
        s.root.on = true;
        s.root.rot[0] = -90;
        lg::Compose(pal, s, out);
        float pivot[3], top[3];
        lg::PivotWorld(pal, 0, pivot);
        const float head[3] = {0, 15, 0};
        World(out, 4, head, top);
        Expect(std::fabs(top[1] - pivot[1]) < 0.4f && std::fabs(top[2] - pivot[2]) > 14.f,
               "prone: the top of the head lies level with the pivot: " + V(top) + " pivot " + V(pivot));
        float p0[3];
        lg::PivotWorld(out, 0, p0);
        Expect(Near(p0, pivot), "the main pivot stays where it was");
    }
    {  // moves and targets
        lg::Spec s;
        s.root.on = true;
        s.root.move[1] = 6;
        s.group[lg::kRightHand].on = true;
        s.group[lg::kRightHand].hasTarget = true;
        const float target[3] = {90, 70, -10};
        std::memcpy(s.group[lg::kRightHand].target, target, sizeof target);
        lg::Compose(pal, s, out);
        float wrist[3], main0[3], ref[3];
        lg::PivotWorld(out, 26, wrist);
        Expect(Near(wrist, target), "the right wrist lands on its target: " + V(wrist));
        lg::PivotWorld(out, 0, main0);
        lg::PivotWorld(pal, 0, ref);
        Expect(std::fabs(main0[1] - ref[1] - 6) < 1e-3f, "root move lifts the body");
        float finger[3], fingerRef[3];
        lg::PivotWorld(out, 30, finger);
        lg::PivotWorld(pal, 30, fingerRef);
        const float d1[3] = {finger[0] - wrist[0], finger[1] - wrist[1], finger[2] - wrist[2]};
        float wr[3];
        lg::PivotWorld(pal, 26, wr);
        const float d0[3] = {fingerRef[0] - wr[0], fingerRef[1] - wr[1], fingerRef[2] - wr[2]};
        Expect(Near(d1, d0), "the hand moves rigidly with its wrist");
    }
    {  // stump: the head subtree flattens onto the neck plane
        lg::Spec s;
        s.stump = true;
        lg::Compose(pal, s, out);
        float neck[3], up[3], top[3], tail[3], tailRef[3];
        lg::NeckFrame(pal, lg::Identity(), neck, up);
        const float head[3] = {0, 18, 0};
        World(out, 4, head, top);
        Expect(std::fabs(top[1] - neck[1]) < 0.5f, "stump: the top of the head ends on the neck plane: " + V(top) + " neck " + V(neck));
        lg::PivotWorld(out, 3, tail);
        lg::PivotWorld(pal, 3, tailRef);
        Expect(Near(tail, tailRef), "stump leaves the body alone");
    }
    {  // the severed head copy
        const float pos[3] = {130, 40, -20};
        const lg::M4 h = lg::Mul(lg::Rotate(0, 0, 90), lg::Translate(pos));
        lg::ComposeCopy(pal, h, out);
        float neck[3], up[3], n2[3], tail[3];
        lg::NeckFrame(pal, lg::Identity(), neck, up);
        World(out, 4, lg::kNeck, n2);
        Expect(Near(n2, pos), "copy: the head's neck point is carried to pos: " + V(n2));
        lg::PivotWorld(out, 3, tail);
        Expect(Near(tail, pos), "copy: the body collapses into the cut: " + V(tail));
        const float eye[3] = {0, 15, 0};
        float e[3];
        World(out, 4, eye, e);
        Expect(std::fabs(e[1] - pos[1]) < 1.f && std::fabs(e[0] - pos[0]) > 10.f, "copy: roll 90 lays the head on its side: " + V(e));
    }
    {  // identification
        const float feet[3][3] = {{0, 0, 0}, {100, 50 - lg::kCentreY, -20}, {140, 30, -20}};
        Expect(lg::Identify(pal, feet, 3, 26.f) == 1, "the draw is the worm whose feet sit under the main pivot");
        const float far[1][3] = {{400, 0, 0}};
        Expect(lg::Identify(pal, far, 1, 26.f) == -1, "no worm within reach: none");
    }
    {  // the learned centre height: unambiguous draws only
        lg::Centre c;
        const float feet[2][3] = {{0, 0, 0}, {200, 0, 0}};
        const float p[3] = {1, 23, -1};
        for (int k = 0; k < 40; ++k) c.Sample(p, feet, 2);
        Expect(std::fabs(c.y - 23.f) < 0.05f && c.samples == 40, "the centre converges on the measured height");
        const float close[2][3] = {{0, 0, 0}, {20, 0, 0}};
        lg::Centre c2;
        Expect(!c2.Sample(p, close, 2) && c2.samples == 0 && c2.y == lg::kCentreY, "a worm 20 units away: no sample");
        const float high[3] = {0, 90, 0};
        Expect(!c2.Sample(high, feet, 2), "a height outside 0..40 is no sample");
        // The mount: two worms 18 apart, the main pivot really 23 over the feet. The guess (19) would still pick
        // the right one here; a wrong guess of 40 would not, the learned value does.
        float pal2[lg::kFloats];
        const float at2[3] = {18, 23 + 0.827f, 0.f - 0.329f};
        Palette(pal2, at2, 0);
        const float pair[2][3] = {{0, 0, 0}, {18, 0, 0}};
        Expect(lg::Identify(pal2, pair, 2, 26.f, c.y) == 1, "identification with the learned centre");
    }
    {  // head attachments
        float head[lg::kHeadBones][3];
        for (int b = 0; b < lg::kHeadBones; ++b) lg::PivotWorld(pal, lg::kHeadFirst + b, head[b]);
        const float hat[3] = {at[0] + 1.f, at[1] + 22.f, at[2] + 4.f};
        const float other[3] = {at[0] + 25.f, at[1] + 20.f, at[2]};
        Expect(lg::NearHead(hat, head) && !lg::NearHead(other, head), "a hat on the head matches, another worm's does not");
        lg::Spec s;
        s.root.on = true;
        s.root.rot[0] = -90;
        lg::M4 d;
        lg::Compose(pal, s, out, &d);
        float moved[3], expect[3];
        lg::Point(d, hat, moved);
        const float bind[3] = {hat[0] - at[0], hat[1] - at[1], hat[2] - at[2]};
        World(out, 4, bind, expect);
        Expect(Near(moved, expect, 1e-2f), "a following attachment moves with the posed head: " + V(moved) + " " + V(expect));
        lg::Spec empty;
        Expect(empty.Empty() && !s.Empty(), "an empty spec only tracks");
        empty.attach = lg::Attach::Hide;
        Expect(!empty.Empty(), "an attachment rule is not empty");
    }
    {  // groups and names
        Expect(lg::GroupOf(0) == -1 && lg::GroupOf(4) == lg::kHead && lg::GroupOf(15) == lg::kHead &&
                   lg::GroupOf(2) == lg::kTail && lg::GroupOf(17) == lg::kLeftHand && lg::GroupOf(33) == lg::kRightHand,
               "bone groups follow the W4.Worm palette");
        Expect(std::strcmp(lg::kBoneNames[26], "wrist_right") == 0 && std::strcmp(lg::kBoneNames[4], "head") == 0,
               "bone names");
        Expect(lg::kPivot[17][0] == -lg::kPivot[26][0] && lg::kPivot[17][1] == lg::kPivot[26][1], "the wrists mirror");
    }
    {  // the CPU route, layer A: the engine's Euler order (R = Ry * Rx * Rz, pitch in the worm's frame after the heading)
        const float origin[3] = {10, 5, -3}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1}, side[3] = {1, 0, 0};
        float p[3];
        lg::BodyPoint(origin, 90, 0, 0, up, p);
        const float face[3] = {10, 5, -2};
        Expect(Near(p, face), "body pitch +90 tips the worm forward onto its face: " + V(p));
        lg::BodyPoint(origin, -90, 0, 0, up, p);
        const float back[3] = {10, 5, -4};
        Expect(Near(p, back), "body pitch -90 lays it on its back, head behind it: " + V(p));
        lg::BodyPoint(origin, 90, 90, 0, up, p);
        const float alongX[3] = {11, 5, -3};
        Expect(Near(p, alongX), "heading 90 + pitch 90: the head lies along the facing (+X), as seen top-down: " + V(p));
        lg::BodyPoint(origin, 0, 90, 0, fwd, p);
        Expect(Near(p, alongX), "heading 90 faces +X (sin, 0, cos), like worms()[i].yaw");
        lg::BodyPoint(origin, 30, 0, 0, fwd, p);
        Expect(std::fabs(p[1] - (5.f - 0.5f)) < 1e-3f && std::fabs(p[2] - (-3.f + 0.8660254f)) < 1e-3f,
               "a forward lean takes the facing down: " + V(p));
        lg::BodyPoint(origin, 30, 0, 0, side, p);
        const float sideOut[3] = {11, 5, -3};
        Expect(Near(p, sideOut), "a pitch keeps the worm's right axis");
        // The feet stay where they are under any turn: the pivot is the mesh origin.
        const float zero[3] = {0, 0, 0};
        lg::BodyPoint(origin, -90, 123, 0, zero, p);
        Expect(Near(p, origin), "the turn is about the feet (the mesh origin)");

        lg::Body b;
        b.on = b.turn = true;
        b.pitch = -90, b.yaw = 10;
        float e[3];
        lg::BodyEuler(b, 1.0f, e);
        Expect(std::fabs(e[0] + 1.5707963f) < 1e-5f && std::fabs(e[1] - (1.0f + 0.17453293f)) < 1e-5f && e[2] == 0,
               "SetRotation gets radians, the yaw as a delta on the drawn yaw");
        b.absHeading = true;
        lg::BodyEuler(b, 1.0f, e);
        Expect(std::fabs(e[1] - 0.17453293f) < 1e-5f, "heading replaces the drawn yaw");
        const float drawn[3] = {100, 53, 7};
        float o[3];
        b.move[0] = 1, b.move[1] = 4, b.move[2] = -2;
        lg::BodyOrigin(b, drawn, o);
        const float moved[3] = {101, 57, 5};
        Expect(Near(o, moved), "move is a delta on the drawn origin");
        b.absAt = true;
        lg::BodyOrigin(b, drawn, o);
        const float at3[3] = {1, 4 + lg::kDrawLift, -2};
        Expect(Near(o, at3), "at is the logic feet, drawn kDrawLift above");
    }
    {  // the CPU route, layer B: clamps and the entry table
        Expect(lg::ClampWeight(-1) == 0 && lg::ClampWeight(2.5f) == 2.5f && lg::ClampWeight(20) == lg::kMaxClipWeight &&
                   lg::ClampWeight(std::nanf("")) == 0,
               "clip weights clamp to 0..5 (NaN -> 0)");
        Expect(lg::ClampSlider(-3) == 0 && lg::ClampSlider(1.2f) == 1.2f && lg::ClampSlider(9) == 2, "sliders clamp to 0..2");
        Expect(lg::ClampTime(-1) == 0 && lg::ClampTime(1e9f) == 600, "clip times clamp to 0..600 s");
        float t[6 * 3];
        for (int i = 0; i < 6; ++i) t[i * 3] = 0.5f, t[i * 3 + 1] = 0.25f, t[i * 3 + 2] = 1;
        t[4 * 3] = 0;  // a slider the game had off
        lg::Clips c;
        c.n = 2;
        c.id[0] = 2, c.time[0] = 0.7f, c.weight[0] = 2;
        c.id[1] = 99, c.time[1] = 1, c.weight[1] = 1;  // out of the table: ignored
        c.sn = 1;
        c.sid[0] = 4, c.st[0] = 1.4f;
        lg::ApplyClips(c, t, 6);
        Expect(t[2 * 3] == 2 && t[2 * 3 + 1] == 0.7f && t[2 * 3 + 2] == 0, "our clip is held at its time and weight");
        Expect(t[4 * 3] == 1 && t[4 * 3 + 1] == 1.4f && t[4 * 3 + 2] == 0, "a slider takes our time (weight 1 if it was off)");
        Expect(t[0] == 0.5f && t[1 * 3 + 1] == 0.25f && t[5 * 3 + 2] == 1, "layer mode keeps the game's clips");
        c.base = true, c.baseId = 1;
        lg::ApplyClips(c, t, 6);
        Expect(t[0] == 0 && t[1 * 3] == 0.5f && t[3 * 3] == 0 && t[2 * 3] == 2 && t[4 * 3] == 1,
               "base mode keeps only Base, our clips and the sliders");
        lg::Clips none;
        Expect(!none.On() && c.On(), "an empty clip edit is off");
    }
    {  // the CPU route, layer C: which worm a skinned draw is, under a moved body (the mount)
        lg::SkinKeys k;
        const unsigned victimKey = 0x1000, attackerKey = 0x2000;
        const int slots[2] = {3, 7};
        // Unposed and apart: both keys are learned.
        float centres[2][3] = {{0, 3, 0}, {40, 3, 0}};
        const float pv[3] = {0.2f, 3.1f, 0}, pa[3] = {40, 2.9f, 0.3f};
        Expect(k.Resolve(victimKey, pv, centres, slots, 2, 1) == 0 && k.Resolve(attackerKey, pa, centres, slots, 2, 1) == 1,
               "apart, the nearest worm");
        Expect(k.Find(victimKey) >= 0 && k.Find(attackerKey) >= 0, "unambiguous draws teach the keys");
        // The mount: the attacker drawn 8 units from the victim, its main pivot off its drawn origin by a clip lean.
        centres[0][0] = 0, centres[0][1] = 7, centres[0][2] = 0;   // the victim, lying, raised by 4
        centres[1][0] = 8, centres[1][1] = 9.5f, centres[1][2] = 0;  // the attacker on its belly
        const float pa2[3] = {3.5f, 9, 0};                        // nearer the victim's centre than its own
        const float pv2[3] = {0.5f, 7, 0};
        Expect(k.Resolve(attackerKey, pa2, centres, slots, 2, 50) == 1, "the attacker's key holds on top of the victim");
        Expect(k.Resolve(victimKey, pv2, centres, slots, 2, 50) == 0, "and the victim's");
        Expect(k.Resolve(0x3000, pa2, centres, slots, 2, 50) == 0 && k.Find(0x3000) < 0,
               "an unknown key falls back to the nearest and is not learned when two worms are close");
        // A key far from its worm (that worm is gone, or a new match reused the address) is not trusted.
        const float far[3] = {200, 3, 0};
        Expect(k.Resolve(attackerKey, far, centres, slots, 2, 60) == -1, "too far from every worm: not identified");
        k.Forget(7);
        Expect(k.Find(attackerKey) < 0 && k.Find(victimKey) >= 0, "forgetting a slot drops only its key");
        // A key learned for one slot and later seen unambiguously on another is moved.
        float c2[2][3] = {{0, 3, 0}, {60, 3, 0}};
        const float onOther[3] = {60, 3, 0};
        Expect(k.Resolve(victimKey, onOther, c2, slots, 2, 70) == 1 && k.e[k.Find(victimKey)].slot == 7,
               "a key re-learned on another worm moves to it");
    }
    printf("pose_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
