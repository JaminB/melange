// Self-tests for src/render/input_logic.h. No game, log or ImGui dependencies (see selftest.h).
#include "render/selftest.h"

#include <cstdio>
#include <vector>

#include "render/input_logic.h"

namespace wf::render {
namespace {
struct Ctx {
    int checks = 0, failed = 0;
    std::string* report;
    void Check(bool ok, const char* what) {
        ++checks;
        if (ok) return;
        ++failed;
        if (report) {
            *report += "FAIL: ";
            *report += what;
            *report += "\n";
        }
    }
};

DIDEVICEOBJECTDATA Rec(uint8_t dik, bool down, DWORD seq) {
    DIDEVICEOBJECTDATA d{};
    d.dwOfs = dik;
    d.dwData = down ? 0x80 : 0;
    d.dwTimeStamp = 1000 + seq;
    d.dwSequence = seq;
    return d;
}

bool Is(const DIDEVICEOBJECTDATA& d, uint8_t dik, bool down) { return d.dwOfs == dik && ((d.dwData & 0x80) != 0) == down; }

constexpr uint8_t kGrave = 0x29, kRight = 0xCD, kLeft = 0xCB, kA = 0x1E, kO = 0x18, kF11 = 0x57, kDown = 0xD0;

void TestParse(Ctx& c) {
    uint8_t d = 0, m = 0;
    c.Check(ParseHotkeyText("GRAVE", &d, &m) && d == kGrave && m == 0, "parse GRAVE");
    c.Check(ParseHotkeyText("Shift+GRAVE", &d, &m) && d == kGrave && m == kModShift, "parse Shift+GRAVE");
    c.Check(ParseHotkeyText("Ctrl+Shift+F11", &d, &m) && d == kF11 && m == (kModCtrl | kModShift), "parse Ctrl+Shift+F11");
    c.Check(ParseHotkeyText(" ctrl + o ", &d, &m) && d == kO && m == kModCtrl, "parse ' ctrl + o '");
    c.Check(ParseHotkeyText("Control+Alt+DELETE", &d, &m) && d == 0xD3 && m == (kModCtrl | kModAlt), "parse Control+Alt+DELETE");
    c.Check(ParseHotkeyText("`", &d, &m) && d == kGrave && m == 0, "parse backtick");
    c.Check(ParseHotkeyText("Shift+0x29", &d, &m) && d == kGrave && m == kModShift, "parse hex");
    c.Check(ParseHotkeyText("SHIFT", &d, &m) && d == kDikLShift && m == 0, "parse lone modifier as key");
    c.Check(!ParseHotkeyText("", &d, &m), "reject empty");
    c.Check(!ParseHotkeyText(nullptr, &d, &m), "reject null");
    c.Check(!ParseHotkeyText("Ctrl+", &d, &m), "reject 'Ctrl+'");
    c.Check(!ParseHotkeyText("Bogus", &d, &m), "reject unknown key");
    c.Check(!ParseHotkeyText("Hyper+O", &d, &m), "reject unknown modifier");
    c.Check(!ParseHotkeyText("0x0", &d, &m) && !ParseHotkeyText("0x100", &d, &m) && !ParseHotkeyText("0xZZ", &d, &m),
            "reject bad hex");
    c.Check(HotkeyLabel(kF11, kModCtrl | kModShift) == "Ctrl+Shift+F11", "label Ctrl+Shift+F11");
    c.Check(HotkeyLabel(kGrave, 0) == "GRAVE", "label GRAVE");
}

void TestFilter(Ctx& c) {
    const HotkeyDef hk[] = {{1, kGrave, 0}, {2, kGrave, kModShift}, {3, kO, kModCtrl}};
    std::vector<int> fired;
    std::vector<uint8_t> rel;
    {  // hotkey removed (down and up), other keys pass, game never sees GRAVE
        KeyFilter f;
        DIDEVICEOBJECTDATA b[8] = {Rec(kRight, true, 1), Rec(kGrave, true, 2), Rec(kGrave, false, 3), Rec(kA, true, 4)};
        DWORD n = f.Process(b, 4, 8, false, hk, 3, &fired, &rel, 0);
        c.Check(n == 2 && Is(b[0], kRight, true) && Is(b[1], kA, true), "hotkey down+up removed, others kept");
        c.Check(fired.size() == 1 && fired[0] == 1, "plain GRAVE fires handle 1 only");
        c.Check(!f.GameDown(kGrave) && f.GameDown(kRight), "game state tracks passed keys");
    }
    {  // exact modifier match: Shift+GRAVE fires 2, not 1; the shift itself reaches the game
        KeyFilter f;
        fired.clear();
        DIDEVICEOBJECTDATA b[8] = {Rec(kDikLShift, true, 1), Rec(kGrave, true, 2)};
        DWORD n = f.Process(b, 2, 8, false, hk, 3, &fired, &rel, 0);
        c.Check(n == 1 && Is(b[0], kDikLShift, true), "shift passes, GRAVE removed");
        c.Check(fired.size() == 1 && fired[0] == 2, "Shift+GRAVE fires handle 2");
        c.Check(f.Mods() == kModShift, "mods tracked");
        // key-up of GRAVE arrives in a later poll: still removed
        DIDEVICEOBJECTDATA b2[4] = {Rec(kGrave, false, 3), Rec(kDikLShift, false, 4)};
        n = f.Process(b2, 2, 4, false, hk, 3, &fired, &rel, 0);
        c.Check(n == 1 && Is(b2[0], kDikLShift, false), "eaten key-up removed in a later poll");
        c.Check(f.Mods() == 0, "mods released");
    }
    {  // right ctrl counts as Ctrl
        KeyFilter f;
        fired.clear();
        DIDEVICEOBJECTDATA b[4] = {Rec(kDikRCtrl, true, 1), Rec(kO, true, 2), Rec(kO, false, 3)};
        DWORD n = f.Process(b, 3, 4, false, hk, 3, &fired, &rel, 0);
        c.Check(n == 1 && fired.size() == 1 && fired[0] == 3, "RCtrl+O fires handle 3");
    }
    {  // stuck keys: the game saw RIGHT down; capture starts -> synthetic RIGHT up once; real up is dropped
        KeyFilter f;
        fired.clear();
        rel.clear();
        DIDEVICEOBJECTDATA b[8] = {Rec(kRight, true, 10)};
        f.Process(b, 1, 8, false, hk, 3, &fired, &rel, 0);
        DWORD n = f.Process(b, 0, 8, true, hk, 3, &fired, &rel, 5000);
        c.Check(n == 1 && Is(b[0], kRight, false) && b[0].dwSequence == 11 && b[0].dwTimeStamp == 5000,
                "synthetic release appended on capture start");
        c.Check(rel.size() == 1 && rel[0] == kRight && f.Synthetic() == 1, "release reported");
        DIDEVICEOBJECTDATA b2[8] = {Rec(kRight, false, 12), Rec(kLeft, true, 13)};
        n = f.Process(b2, 2, 8, true, hk, 3, &fired, &rel, 0);
        c.Check(n == 0 && f.Dropped() == 2, "everything dropped while capturing");
        DIDEVICEOBJECTDATA b3[8] = {Rec(kGrave, true, 14)};
        n = f.Process(b3, 1, 8, true, hk, 3, &fired, &rel, 0);
        c.Check(n == 0 && fired.size() == 1 && fired[0] == 1, "hotkeys still fire while capturing");
        n = f.Process(b3, 0, 8, false, hk, 3, &fired, &rel, 0);
        c.Check(n == 0 && rel.size() == 1, "capture end sends nothing");
    }
    {  // synthetic releases respect the buffer capacity and continue on the next poll
        KeyFilter f;
        rel.clear();
        DIDEVICEOBJECTDATA b[8] = {Rec(kRight, true, 1), Rec(kLeft, true, 2), Rec(kDown, true, 3)};
        f.Process(b, 3, 8, false, hk, 3, nullptr, nullptr, 0);
        DWORD n = f.Process(b, 0, 2, true, hk, 3, nullptr, &rel, 0);
        c.Check(n == 2 && rel.size() == 2, "capacity-limited releases (first poll)");
        n = f.Process(b, 0, 2, true, hk, 3, nullptr, &rel, 0);
        c.Check(n == 1 && rel.size() == 3, "remaining release on the next poll");
        n = f.Process(b, 0, 2, true, hk, 3, nullptr, &rel, 0);
        c.Check(n == 0, "no repeat releases");
    }
    {  // GetDeviceState filtering
        KeyFilter f;
        DIDEVICEOBJECTDATA b[4] = {Rec(kGrave, true, 1), Rec(kA, true, 2)};
        f.Process(b, 2, 4, false, hk, 3, nullptr, nullptr, 0);
        uint8_t st[256] = {};
        st[kGrave] = 0x80;
        st[kA] = 0x80;
        f.FilterState(st, 256, false);
        c.Check(st[kGrave] == 0 && st[kA] == 0x80, "held hotkey hidden from GetDeviceState");
        f.FilterState(st, 256, true);
        c.Check(st[kA] == 0, "GetDeviceState zeroed while capturing");
    }
    {  // device reset forgets held keys
        KeyFilter f;
        DIDEVICEOBJECTDATA b[4] = {Rec(kDikLCtrl, true, 1)};
        f.Process(b, 1, 4, false, hk, 3, nullptr, nullptr, 0);
        f.ResetDevice();
        c.Check(f.Mods() == 0 && !f.GameDown(kDikLCtrl), "ResetDevice clears state");
    }
}

void TestMenu(Ctx& c) {
    std::vector<std::string> s;
    c.Check(SplitMenuPath("File/Save logs as...", s) && s.size() == 2 && s[0] == "File" && s[1] == "Save logs as...",
            "split File/Save logs as...");
    c.Check(SplitMenuPath(" a // b /", s) && s.size() == 2 && s[0] == "a" && s[1] == "b", "split trims and drops empties");
    c.Check(SplitMenuPath("Solo", s) && s.size() == 1, "split single");
    c.Check(!SplitMenuPath("", s) && !SplitMenuPath(" / ", s) && !SplitMenuPath(nullptr, s), "split rejects empty");
}
}  // namespace

int RunLogicSelfTests(std::string* report) {
    Ctx c;
    c.report = report;
    TestParse(c);
    TestFilter(c);
    TestMenu(c);
    if (report) {
        char b[96];
        snprintf(b, sizeof(b), "overlay logic self-test: %d/%d checks passed", c.checks - c.failed, c.checks);
        *report += b;
    }
    return c.failed;
}
}  // namespace wf::render
