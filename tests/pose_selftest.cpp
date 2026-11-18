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
    printf("pose_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
