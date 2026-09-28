// Offline self-test of the draw queue (render/mirage/draw_queue.h).
#include <iostream>

#include "render/mirage/draw_queue.h"

namespace {
using melange::draw::kDepthTest;
using melange::mirage::drawqueue::Kind;
using melange::mirage::drawqueue::Primitive;
using melange::mirage::drawqueue::RouteStage;
using melange::mirage::drawqueue::StageQueue;
using melange::mirage::drawqueue::Unpack;
using melange::render::Stage;

int g_checks = 0, g_failures = 0;
void Check(bool cond, const char* what) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
}

Primitive Make(int frames) {
    Primitive p;
    p.kind = Kind::Line;
    p.framesLeft = frames;
    return p;
}

void TestOneShotDrop() {
    StageQueue q;
    q.Push(Make(1), false);
    Check(!q.Idle(), "one-shot: queued before its first pass is not idle");

    auto& pass = q.BeginPass();
    Check(pass.size() == 1, "one-shot: BeginPass promotes the queued primitive");
    Check(q.Immediate().empty(), "one-shot: nothing landed in the immediate list");
    q.EndPass();
    Check(q.Idle(), "one-shot: frames=1 is dropped after its one pass");
}

void TestMultiFrameSurvives() {
    StageQueue q;
    q.Push(Make(3), false);

    for (int pass = 0; pass < 3; ++pass) {
        auto& v = q.BeginPass();
        Check(v.size() == 1, "multi-frame: exactly one primitive drawn each of its 3 passes");
        q.EndPass();
    }
    Check(q.Idle(), "multi-frame: gone after exactly 3 passes");
}

void TestImmediateSameFrame() {
    StageQueue q;
    q.BeginPass();
    Check(q.Immediate().empty() && q.BeginPass().empty(), "immediate: window opens empty");
    q.Push(Make(1), /*insideThisStage=*/true);
    Check(q.Immediate().size() == 1, "immediate: an inside-window push lands in Immediate(), not next pass's queue");
    q.EndPass();
    Check(q.Idle(), "immediate: a one-shot immediate primitive is gone once its pass ends");
}

void TestOutsidePushDuringOpenWindowWaitsOnePass() {
    StageQueue q;
    q.BeginPass();
    q.Push(Make(1), /*insideThisStage=*/false);
    Check(q.Immediate().empty(), "outside-during-window: does not join this pass's immediate list");
    q.EndPass();
    Check(!q.Idle(), "outside-during-window: still pending after this pass ends");
    auto& v = q.BeginPass();
    Check(v.size() == 1, "outside-during-window: drawn on the following pass");
    q.EndPass();
}

void TestRouting() {
    Check(RouteStage(Kind::Line, kDepthTest) == Stage::World, "routing: depth-tested world primitive -> World");
    Check(RouteStage(Kind::Line, 0) == Stage::WorldLate, "routing: non-depth-tested world primitive -> WorldLate");
    Check(RouteStage(Kind::HudRect, kDepthTest) == Stage::Hud, "routing: HUD kinds ignore flags and always go to Hud");
}

void TestColorUnpack() {
    float rgba[4];
    Unpack(0xff0000ffu, rgba);  // IM_COL32(255,0,0,255): opaque red
    Check(rgba[0] > 0.99f && rgba[1] < 0.01f && rgba[2] < 0.01f && rgba[3] > 0.99f, "colour: opaque red unpacks R=1,A=1");
    Unpack(0x80ff0000u, rgba);  // IM_COL32(0,0,255,128): half-alpha blue
    Check(rgba[2] > 0.99f && rgba[0] < 0.01f && rgba[3] > 0.49f && rgba[3] < 0.51f, "colour: half-alpha blue unpacks B=1,A~0.5");
}

}  // namespace

int main() {
    TestOneShotDrop();
    TestMultiFrameSurvives();
    TestImmediateSameFrame();
    TestOutsidePushDuringOpenWindowWaitsOnePass();
    TestRouting();
    TestColorUnpack();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
