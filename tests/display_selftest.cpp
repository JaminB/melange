// Offline self-test of borderless fullscreen's pure logic: local.cfg read/rewrite and the window-size list
// (launcher/local_cfg.h), the window rect and style math (render/display_logic.h), the Alt+Enter hotkey and the
// Alt-release guard (render/input_logic.h), and the scene-target size fallback (render/mirage/supersample_logic.h).
#include <cstdio>
#include <string>
#include <vector>

#include "launcher/local_cfg.h"
#include "render/display_logic.h"
#include "render/input_logic.h"
#include "render/mirage/supersample_logic.h"

namespace {
namespace lc = melange::launcher::localcfg;
namespace dl = melange::display::logic;
namespace ss = melange::mirage::supersample::logic;

struct Ctx {
    int checks = 0, failed = 0;
    void Check(bool ok, const char* what, const std::string& got = {}) {
        ++checks;
        if (ok) return;
        ++failed;
        std::printf("FAIL: %s%s%s\n", what, got.empty() ? "" : " -> ", got.c_str());
    }
};

const char* kStock = "/W:1280 /H:720 /REFRESH:59 /SSAA:1 /SHADOWMAP:1024 /CONFIG:user.cfg";

void TestRead(Ctx& c) {
    lc::Info i = lc::Read(kStock);
    c.Check(i.w == 1280 && i.h == 720 && !i.fs && !i.win, "read: the stock line");
    i = lc::Read("/FS /W:1920 /H:1080\r\n");
    c.Check(i.fs && i.w == 1920 && i.h == 1080, "read: /FS and the size");
    i = lc::Read("/w 1600 /h 900 /WIN");
    c.Check(i.w == 1600 && i.h == 900 && i.win && !i.fs, "read: lower case, spaced values, /WIN");
    i = lc::Read("/W:800 /H:600 /W:1024 /H:768");
    c.Check(i.w == 1024 && i.h == 768, "read: the last one wins");
    i = lc::Read("/WIN /W:abc /H:");
    c.Check(i.w == 0 && i.h == 0, "read: /WIN is not /W, and a non-number is ignored");
    c.Check(lc::Read("").w == 0, "read: empty");
}

void TestRewrite(Ctx& c) {
    bool removed = true;
    std::string s = lc::Rewrite(kStock, 1920, 1080, false, &removed);
    c.Check(s == "/W:1920 /H:1080 /REFRESH:59 /SSAA:1 /SHADOWMAP:1024 /CONFIG:user.cfg" && !removed, "rewrite: the size in place", s);
    s = lc::Rewrite("/W:1280 /FS /H:720 /CONFIG:user.cfg\r\n", 2560, 1440, true, &removed);
    c.Check(s == "/W:2560 /H:1440 /CONFIG:user.cfg\r\n" && removed, "rewrite: /FS in the middle goes, CRLF kept", s);
    s = lc::Rewrite("/FS /W:1280 /H:720", 1280, 720, true, &removed);
    c.Check(s == "/W:1280 /H:720" && removed, "rewrite: /FS first", s);
    s = lc::Rewrite("/W:1280 /H:720 /FS\n", 1280, 720, true, &removed);
    c.Check(s == "/W:1280 /H:720\n" && removed, "rewrite: /FS last", s);
    s = lc::Rewrite("/W:1280 /H:720 /FS", 1280, 720, false, &removed);
    c.Check(s == "/W:1280 /H:720 /FS" && !removed, "rewrite: /FS kept when fullscreen is off", s);
    s = lc::Rewrite("/REFRESH:60 /SSAA:4\r\n", 1600, 900, false);
    c.Check(s == "/REFRESH:60 /SSAA:4 /W:1600 /H:900\r\n", "rewrite: missing /W /H appended before the newline", s);
    s = lc::Rewrite("", 1280, 720, true);
    c.Check(s == "/W:1280 /H:720", "rewrite: empty file", s);
    s = lc::Rewrite("/FS\r\n", 1280, 720, true, &removed);
    c.Check(s == "/W:1280 /H:720\r\n" && removed, "rewrite: only /FS", s);
    s = lc::Rewrite("/w 1280 /h 720 /SSAA 1", 1920, 1080, false);
    c.Check(s == "/w 1920 /h 1080 /SSAA 1", "rewrite: spaced values keep their style and spelling", s);
    s = lc::Rewrite("/W 1280 /SSAA:1", 1920, 1080, false);
    c.Check(s == "/W 1920 /SSAA:1 /H 1080", "rewrite: an added /H follows the spaced style", s);
    s = lc::Rewrite("/WIN /W:1280 /H:720 /UNKNOWN:7 /FOO", 1366, 768, true);
    c.Check(s == "/WIN /W:1366 /H:768 /UNKNOWN:7 /FOO", "rewrite: /WIN and unknown switches untouched", s);
    s = lc::Rewrite("/W: /H:720", 800, 600, false);
    c.Check(s == "/W:800 /H:600", "rewrite: an empty value is filled", s);
    c.Check(lc::Read(lc::Rewrite(kStock, 3840, 2160, true)).w == 3840, "rewrite: reads back");
}

void TestModes(Ctx& c) {
    std::vector<lc::Size> rep = {{640, 480}, {800, 600}, {1920, 1080}, {1920, 1080}, {1600, 1200}, {2560, 1440}, {320, 200}};
    std::vector<lc::Size> m = lc::Modes(rep, 1920, 1080);
    c.Check(!m.empty() && m.front() == lc::Size{1920, 1080}, "modes: largest first");
    bool over = false, dup = false, has720 = false, tall = false, tiny = false;
    for (size_t i = 0; i < m.size(); ++i) {
        if (m[i].w > 1920 || m[i].h > 1080) over = true;
        if (m[i].w < 640) tiny = true;
        if (m[i] == lc::Size{1280, 720}) has720 = true;
        if (m[i] == lc::Size{1600, 1200}) tall = true;
        for (size_t j = i + 1; j < m.size(); ++j)
            if (m[i] == m[j]) dup = true;
    }
    c.Check(!over && !dup && !tiny, "modes: within the monitor, no duplicates, nothing under 640x480");
    c.Check(has720, "modes: common 16:9 sizes are offered");
    c.Check(!tall, "modes: a reported mode taller than the monitor is dropped");
    c.Check(lc::ValidSize(640, 480) && !lc::ValidSize(639, 480) && !lc::ValidSize(20000, 1080), "modes: ValidSize bounds");
}

RECT R(LONG l, LONG t, LONG r, LONG b) { return RECT{l, t, r, b}; }

void TestStyles(Ctx& c) {
    const LONG windowed = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE | WS_CLIPSIBLINGS;
    const LONG b = dl::BorderlessStyle(windowed);
    c.Check((b & WS_POPUP) && (b & WS_VISIBLE) && (b & WS_CLIPSIBLINGS), "style: popup, visible and clip bits kept");
    c.Check(!(b & (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME)), "style: no frame, caption or system menu");
    const LONG ex = dl::BorderlessExStyle(WS_EX_WINDOWEDGE | WS_EX_APPWINDOW | WS_EX_CLIENTEDGE);
    c.Check(ex == WS_EX_APPWINDOW, "ex style: edges gone, APPWINDOW kept");
    const RECT mon = R(0, 0, 1920, 1080);
    c.Check(dl::LooksExclusive(WS_POPUP | WS_VISIBLE, mon, mon), "exclusive: a captionless window covering the monitor");
    c.Check(!dl::LooksExclusive(windowed, R(-8, -31, 1928, 1088), mon), "exclusive: a captioned window larger than the monitor is not");
    c.Check(!dl::LooksExclusive(WS_POPUP, R(0, 0, 1280, 720), mon), "exclusive: a small popup is not");
}

void TestRestore(Ctx& c) {
    const RECT primary = R(0, 0, 1920, 1040), left = R(-2560, 0, 0, 1400);
    const RECT saved = R(-1800, 100, -520, 851);  // a 1280x751 window on the monitor left of the primary
    RECT r = dl::RestoreRect(saved, {primary, left}, primary);
    c.Check(dl::SameRect(r, saved), "restore: the saved rect on a monitor at negative coordinates");
    r = dl::RestoreRect(saved, {primary}, primary);  // that monitor was unplugged
    c.Check(dl::Width(r) == 1280 && dl::Height(r) == 751 && r.left == 320 && r.top == 144, "restore: centred on the window's monitor");
    r = dl::RestoreRect(R(-1300, 10, -20, 761), {primary}, primary);  // only 20 px still visible
    c.Check(r.left >= 0, "restore: a rect almost off-screen comes back");
    r = dl::RestoreRect(R(5000, 5000, 7600, 6500), {primary}, primary);  // bigger than the work area
    c.Check(r.left == 0 && r.top == 0 && dl::Width(r) == 2600, "restore: too big: kept inside from the top-left");
    // DPI-unaware: on a 150% 2560x1440 monitor the game sees a 1707x960 one; the math stays in that space.
    const RECT logical = R(0, 0, 1707, 920);
    r = dl::RestoreRect(R(3000, 0, 4280, 751), {logical}, logical);
    c.Check(r.left == 213 && r.top == 84, "restore: virtualized (logical) coordinates");
    c.Check(dl::SameRect(dl::RestoreRect(saved, {}, R(0, 0, 0, 0)), saved), "restore: nothing known: the saved rect");
}

void TestViewport(Ctx& c) {
    const int ok[4] = {0, 0, 1920, 1080}, old[4] = {0, 0, 1280, 720}, off[4] = {0, 360, 1920, 1080};
    c.Check(dl::ViewportMatches(ok, 1920, 1080), "viewport: matches the client area");
    c.Check(!dl::ViewportMatches(old, 1920, 1080) && !dl::ViewportMatches(off, 1920, 1080), "viewport: stale or offset");
}

void TestHotkey(Ctx& c) {
    uint8_t dik = 0, mods = 0;
    c.Check(melange::render::ParseHotkeyText("Alt+RETURN", &dik, &mods) && dik == 0x1C && mods == melange::render::kModAlt,
            "hotkey: Alt+RETURN");
    c.Check(melange::render::HotkeyLabel(dik, mods) == "Alt+RETURN", "hotkey: label round-trips");
    c.Check(melange::render::ParseHotkeyText("alt + enter", &dik, &mods) && dik == 0x1C, "hotkey: alt + enter");
    c.Check(melange::render::ParseHotkeyText("Ctrl+Shift+F11", &dik, &mods) && mods == (melange::render::kModCtrl | melange::render::kModShift),
            "hotkey: other hotkeys still parse");
    c.Check(!melange::render::ParseHotkeyText("Win+RETURN", &dik, &mods), "hotkey: unknown modifier refused");
    c.Check(dl::HotkeyDisabled("") && dl::HotkeyDisabled(" none ") && dl::HotkeyDisabled("OFF") && !dl::HotkeyDisabled("Alt+RETURN"),
            "hotkey: empty, none and off disable it");
}

void TestAltGuard(Ctx& c) {
    melange::render::AltReleaseGuard g;
    const LPARAM fresh = 0, repeat = 1 << 30;
    c.Check(!g.Drop(WM_SYSKEYDOWN, VK_MENU, fresh, false), "alt guard: Alt down passes");
    c.Check(!g.Drop(WM_SYSKEYDOWN, VK_RETURN, fresh, true) && g.Pending(), "alt guard: the hotkey's key is dropped by the caller, Alt release armed");
    c.Check(!g.Drop(WM_SYSKEYDOWN, VK_MENU, repeat, false) && g.Pending(), "alt guard: Alt auto-repeat keeps it armed");
    c.Check(g.Drop(WM_SYSKEYUP, VK_MENU, fresh, false) && !g.Pending(), "alt guard: that Alt release is dropped once");
    c.Check(!g.Drop(WM_SYSKEYUP, VK_MENU, fresh, false), "alt guard: the next Alt release passes");
    g.Drop(WM_SYSKEYDOWN, VK_RETURN, fresh, true);
    c.Check(!g.Drop(WM_SYSKEYDOWN, VK_MENU, fresh, false) && !g.Pending(), "alt guard: a fresh Alt press forgets it");
    c.Check(!g.Drop(WM_KEYUP, 'A', fresh, false), "alt guard: other keys pass");
}

void TestTargets(Ctx& c) {
    const ss::EngineAa x22{2, 2, false, false}, x44{4, 4, false, false}, one{1, 1, false, true}, fx{1, 1, true, true};
    c.Check(ss::FitsSize(x22, 3840, 2160, 16384), "targets: 2x2 at 4K fits 16384");
    c.Check(!ss::FitsSize(x22, 5120, 2880, 8192), "targets: 2x2 at 5K does not fit 8192");
    c.Check(ss::FitsSize(x44, 3840, 2160, 0), "targets: an unknown limit fits");
    c.Check(ss::Same(ss::ForSize(x22, one, 1920, 1080, 8192), x22), "targets: the wanted factors when they fit");
    c.Check(ss::Same(ss::ForSize(x44, x22, 3840, 2160, 8192), x22), "targets: else the current ones");
    const ss::EngineAa r = ss::ForSize(x44, x44, 3840, 2160, 8192);
    c.Check(r.x == 1 && r.y == 1 && !r.fxaa, "targets: else 1x1");
    c.Check(ss::ForSize(x44, fx, 5120, 2880, 4096).fxaa, "targets: FXAA kept when it was on at 1x1");
}
}  // namespace

int main() {
    Ctx c;
    TestRead(c);
    TestRewrite(c);
    TestModes(c);
    TestStyles(c);
    TestRestore(c);
    TestViewport(c);
    TestHotkey(c);
    TestAltGuard(c);
    TestTargets(c);
    std::printf("display_selftest: %d/%d checks passed\n", c.checks - c.failed, c.checks);
    return c.failed ? 1 : 0;
}
