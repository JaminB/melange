// Offline self-test of the Controls module's pure logic: the invert-mode table, the sensitivity carry, key labels,
// and the bare function-key hotkey rule (input/controls_logic.h, render/input_logic.h, core/keys.h).
#include <cmath>
#include <cstdio>
#include <string>

#include "input/controls_logic.h"
#include "render/input_logic.h"

namespace {
namespace ctl = melange::controls;

struct Ctx {
    int checks = 0, failed = 0;
    void Check(bool ok, const char* what, const std::string& got = {}) {
        ++checks;
        if (ok) return;
        ++failed;
        std::printf("FAIL: %s%s%s\n", what, got.empty() ? "" : " -> ", got.c_str());
    }
};

void TestInvert(Ctx& c) {
    ctl::InvertMode m;
    c.Check(ctl::ParseInvertMode("game", &m) && m == ctl::InvertMode::Game, "mode: game");
    c.Check(ctl::ParseInvertMode("standard", &m) && m == ctl::InvertMode::Standard, "mode: standard");
    c.Check(ctl::ParseInvertMode("inverted", &m) && m == ctl::InvertMode::Inverted, "mode: inverted");
    c.Check(!ctl::ParseInvertMode("Standard", &m) && !ctl::ParseInvertMode("", &m) && !ctl::ParseInvertMode(nullptr, &m),
            "mode: unknown strings are rejected");
    for (uint8_t std : {uint8_t{0}, uint8_t{1}}) {
        c.Check(ctl::InvertFlagFor(ctl::InvertMode::Game, std) == -1, "flag: game leaves the game's flag");
        c.Check(ctl::InvertFlagFor(ctl::InvertMode::Standard, std) == std, "flag: standard is the standard constant");
        c.Check(ctl::InvertFlagFor(ctl::InvertMode::Inverted, std) == (std ^ 1), "flag: inverted is the opposite");
    }
    c.Check(ctl::kStandardCameraInvertY == 1 && ctl::kStandardAimInvertY == 0, "flag: initial standard constants");
    c.Check(std::string(ctl::InvertModeName(ctl::InvertMode::Inverted)) == "inverted", "mode: name");
}

void TestCarry(Ctx& c) {
    ctl::Carry k;
    c.Check(k.Apply(7, 1.0f) == 7 && k.residue == 0.0f, "carry: scale 1 is exact");
    int sum = 0;
    for (int i = 0; i < 10; ++i) sum += k.Apply(1, 0.25f);
    c.Check(sum == 2, "carry: ten 0.25 steps give 2 (the remaining 0.5 is kept)", std::to_string(sum));
    c.Check(std::fabs(k.residue - 0.5f) < 1e-6f, "carry: residue 0.5");
    ctl::Carry n;
    sum = 0;
    for (int i = 0; i < 10; ++i) sum += n.Apply(-1, 0.25f);
    c.Check(sum == -2 && std::fabs(n.residue + 0.5f) < 1e-6f, "carry: negative motion truncates toward zero symmetrically");
    ctl::Carry s;
    c.Check(s.Apply(3, 3.0f) == 9 && s.Apply(-4, 0.5f) == -2, "carry: whole results");
    ctl::Carry r;
    int total = 0;
    for (int i = 0; i < 1000; ++i) total += r.Apply(1, 0.3f);
    c.Check(total == 300 || total == 299, "carry: 1000 x 0.3 keeps the total", std::to_string(total));
    c.Check(std::fabs(r.residue) < 1.0f, "carry: residue stays below 1");
    r.Reset();
    c.Check(r.residue == 0.0f, "carry: reset");
}

void TestSens(Ctx& c) {
    c.Check(ctl::ClampSensitivity(0.1) == 0.25f && ctl::ClampSensitivity(9) == 3.0f && ctl::ClampSensitivity(1.5) == 1.5f,
            "sensitivity: clamped to 0.25..3");
    c.Check(ctl::ClampSensitivity(NAN) == 1.0f, "sensitivity: NaN is 1");
}

void TestLabels(Ctx& c) {
    c.Check(ctl::KeyLabel("SPACE") == "Space", "label: SPACE");
    c.Check(ctl::KeyLabel("Q") == "Q", "label: Q");
    c.Check(ctl::KeyLabel("LCONTROL") == "Ctrl" && ctl::KeyLabel("RETURN") == "Enter", "label: special names");
    c.Check(ctl::KeyLabel("F1") == "F1" && ctl::KeyLabel("") .empty() && ctl::KeyLabel(nullptr).empty(), "label: F1, empty, null");
    c.Check(ctl::KeyLabel(melange::render::DikName(0x39)) == "Space", "label: through DikName(0x39)");
}

void TestHotkey(Ctx& c) {
    using namespace melange::render;
    uint8_t d = 0, m = 0;
    for (int i = 1; i <= 12; ++i) {
        const std::string t = "F" + std::to_string(i);
        c.Check(ParseHotkeyText(t.c_str(), &d, &m) && m == 0 && melange::automation::IsFunctionKeyDik(d), t.c_str());
    }
    c.Check(ParseHotkeyText("f9", &d, &m) && melange::automation::IsFunctionKeyDik(d) && m == 0, "hotkey: lower-case f9");
    c.Check(ParseHotkeyText("Shift+F3", &d, &m) && m == kModShift, "hotkey: Shift+F3 unchanged");
    for (const char* t : {"Q", "SPACE", "GRAVE", "0x29", "F13"}) {
        const bool parsed = ParseHotkeyText(t, &d, &m);
        c.Check(!parsed || !melange::automation::IsFunctionKeyDik(d), t);
    }
    c.Check(!melange::automation::IsFunctionKeyDik(0x45) && !melange::automation::IsFunctionKeyDik(0x59) &&
                !melange::automation::IsFunctionKeyDik(0x3A),
            "hotkey: neighbours of the function-key codes are not function keys");
}
}  // namespace

int main() {
    Ctx c;
    TestInvert(c);
    TestCarry(c);
    TestSens(c);
    TestLabels(c);
    TestHotkey(c);
    std::printf("controls_selftest: %d/%d checks passed\n", c.checks - c.failed, c.checks);
    return c.failed ? 1 : 0;
}
