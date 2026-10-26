// Offline self-test for mod input routing: regions and priority, synthetic releases, never swallowing an up whose
// down the game saw, pointer release rules (zero-delta exit), focus against hotkeys, paste and the frontend cursor map.
// Owner: C2 (Melange 0.12.0). The pure ModInput (input_logic.h) and the core (input_regions_core.cpp) on a fake Host.
// Exit code 0 = all passed.
#include <windows.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "input/gesture.h"
#include "render/input_logic.h"
#include "render/input_regions.h"

using namespace melange::render;
using Result = modinput::Result;
using Shape = modinput::Shape;

namespace {
int g_pass = 0, g_fail = 0;

void Expect(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what);
    }
}

void ExpectEq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) printf("  %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
    Expect(got == want, what);
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.6f; }

ModInput::Point P(float x, float y) {
    ModInput::Point p;
    p.valid = true;
    p.x = x;
    p.y = y;
    return p;
}

constexpr int kW = 1920, kH = 1080;

void TestPure() {
    // ---- message decoding
    Expect(DecodeButton(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2)).button == kBtnX2, "x2 decoded");
    Expect(DecodeButton(WM_RBUTTONDBLCLK, 0).down && DecodeButton(WM_RBUTTONDBLCLK, 0).dbl, "a double-click message is a press");
    Expect(DecodeButton(WM_MOUSEMOVE, 0).button == -1, "a move is no button");
    Expect(ButtonUpMsg(kBtnX1) == WM_XBUTTONUP && HIWORD(ButtonUpWParam(kBtnX1)) == XBUTTON1, "x1 release message");

    // ---- JSON and text
    ModEvent e;
    e.kind = ModEvent::Kind::Mouse;
    e.region = "a\"b\\c\n";
    e.button = kBtnMiddle;
    e.down = true;
    e.x = 12.5f;
    e.y = 3;
    ExpectEq(ModEventJson(e), "{\"region\":\"a\\\"b\\\\c\\u000a\",\"button\":\"middle\",\"down\":true,\"x\":12.5,\"y\":3,\"double\":false}",
             "mouse event JSON is escaped");
    e.kind = ModEvent::Kind::Key;
    e.key = "RETURN";
    e.down = false;
    e.ctrl = true;
    ExpectEq(ModEventJson(e), "{\"key\":\"RETURN\",\"down\":false,\"repeat\":false,\"ctrl\":true,\"shift\":false,\"alt\":false}",
             "key event JSON");
    e.kind = ModEvent::Kind::Wheel;
    e.delta = std::nanf("");
    Expect(ModEventJson(e).find("\"delta\":0") != std::string::npos, "NaN never reaches the JSON");
    ExpectEq(ModEventName(ModEvent::Kind::Paste), "melange.input.paste", "event names come from event_names.h");
    ExpectEq(SanitizePaste(L"  a\tb\r\nc\x7f\x85\xd83d\xde00 "), "  abc\xF0\x9F\x98\x80 ", "paste strips control characters");
    ExpectEq(SanitizePaste(L"x\xdc00y\xd800"), "xy", "paste drops unpaired surrogates");
    ExpectEq(SanitizePaste(std::wstring(5000, L'a')).size() == 4096 ? "4096" : "?", "4096", "paste is capped at 4096 bytes");
    ExpectEq(SanitizePaste(std::wstring(2000, L'\x00e9')).size() == 4000 ? "ok" : "?", "ok", "two-byte characters fit whole");
    ExpectEq(SanitizePaste(L"a" + std::wstring(2048, L'\x00e9')).size() == 4095 ? "ok" : "?", "ok",
             "the cap never splits a character");
    ExpectEq(KeyNameOf(0x1C << 16, VK_RETURN), "RETURN", "key name from the scan code");
    ExpectEq(KeyNameOf((0x4B << 16) | (1 << 24), VK_LEFT), DikName(0xCB), "extended keys use the 0x80 DIK");
    ExpectEq(KeyNameOf(0, VK_TAB), "TAB", "key name from the virtual key when there is no scan code");

    // ---- hotkeys against focus
    Expect(!HotkeyLiveWhileFocused(0x41, 0, true), "a bare F7 does not fire while focused");
    Expect(HotkeyLiveWhileFocused(0x41, 0, false), "a bare F7 fires without focus");
    Expect(HotkeyLiveWhileFocused(0x41, kModCtrl | kModShift, true), "Ctrl+Shift+F7 fires while focused");
    Expect(HotkeyLiveWhileFocused(0x19, kModCtrl | kModShift, true), "Ctrl+Shift+P fires while focused");
    Expect(HotkeyLiveWhileFocused(0x29, 0, true), "the overlay's bare GRAVE still fires");
    // the DirectInput side: the Esc stand-in hotkey eats the press and its release; focus capture drops the rest
    {
        KeyFilter f;
        DIDEVICEOBJECTDATA buf[8] = {};
        buf[0].dwOfs = 0x11, buf[0].dwData = 0x80, buf[0].dwSequence = 1;  // W down, the game sees it
        HotkeyDef esc{-1, 0x01, 0};
        std::vector<int> fired;
        std::vector<uint8_t> released;
        Expect(f.Process(buf, 1, 8, false, &esc, 1, &fired, &released, 0) == 1 && f.GameDown(0x11), "W reaches the game");
        buf[0].dwOfs = 0x01, buf[0].dwData = 0x80, buf[0].dwSequence = 2;
        Expect(f.Process(buf, 1, 8, false, &esc, 1, &fired, &released, 0) == 0 && fired == std::vector<int>{-1},
               "a bare Esc is eaten and reported");
        buf[0].dwData = 0, buf[0].dwSequence = 3;
        Expect(f.Process(buf, 1, 8, false, nullptr, 0, &fired, &released, 0) == 0, "its release is eaten too");
        buf[0].dwOfs = 0x1E, buf[0].dwData = 0x80, buf[0].dwSequence = 4;
        const DWORD n = f.Process(buf, 1, 8, true, nullptr, 0, &fired, &released, 0);  // focus taken: capture
        Expect(n == 1 && buf[0].dwOfs == 0x11 && buf[0].dwData == 0 && released == std::vector<uint8_t>{0x11},
               "focus drops A and releases the W the game held");
    }

    // ---- frontend cursor mapping (0x4d4cc0 factors, +y up)
    float x = 0, y = 0;
    Expect(FeCursorToWindow(-426.6667f, 240, 1920, 1080, kW, kH, &x, &y) && Near(x, 0) && Near(y, 0), "16:9 top-left");
    Expect(FeCursorToWindow(426.6667f, -240, 1920, 1080, kW, kH, &x, &y) && Near(x, kW) && Near(y, kH), "16:9 bottom-right");
    Expect(FeCursorToWindow(0, 0, 1920, 1080, kW, kH, &x, &y) && Near(x, kW / 2.0f) && Near(y, kH / 2.0f), "centre");
    Expect(FeCursorToWindow(320, 240, 1024, 768, 1024, 768, &x, &y) && Near(x, 1024) && Near(y, 0), "4:3 top-right");
    Expect(FeCursorToWindow(426.6667f, 0, 2560, 1080, 2560, 1080, &x, &y) && Near(x, 2560), "21:9 uses the 16:9 factor");
    Expect(FeCursorToWindow(-300, -225, 1280, 1024, 1280, 1024, &x, &y) && Near(x, 0) && Near(y, 1024), "5:4 scales both");
    Expect(FeCursorToWindow(0, 0, 0, 0, 800, 600, &x, &y) && Near(x, 400), "no render size: 4:3 like the game");
    Expect(!FeCursorToWindow(5000, 0, 1920, 1080, kW, kH, &x, &y), "an impossible position is rejected");
    Expect(!FeCursorToWindow(std::nanf(""), 0, 1920, 1080, kW, kH, &x, &y), "NaN is rejected");

    // ---- regions: priority, renewal, consent, lapse
    ModInput m;
    m.SetDoubleClick(500, 4, 4);
    uint64_t frame = 10, now = 1000;
    Expect(m.Claim("a", 1, "r1", 100, 100, 0, 0, 2, Shape::Arrow, frame, now, false) == Result::Ok, "a reversed rect is normalised");
    Expect(m.Hit(P(50, 50), frame) && m.Hit(P(50, 50), frame)->id == "r1", "hit inside");
    Expect(!m.Hit(P(100, 50), frame), "the right edge is outside");
    m.Claim("a", 1, "r2", 40, 40, 60, 60, 2, Shape::Hand, frame, now, false);
    Expect(m.Hit(P(50, 50), frame)->id == "r2", "the later claim of a mod is on top");
    m.Claim("a", 1, "r1", 0, 0, 100, 100, 2, Shape::Arrow, frame, now, false);
    Expect(m.Hit(P(50, 50), frame)->id == "r1", "renewing reorders: claim order is draw order");
    Expect(m.Claim("b", 2, "r3", 50, 50, 80, 80, 2, Shape::Arrow, frame, now, false) == Result::Ok, "a newer mod overlaps");
    Expect(m.Hit(P(55, 55), frame)->mod == "b", "the newer mod is on top");
    Expect(m.Claim("a", 1, "r2", 40, 40, 60, 60, 2, Shape::Arrow, frame, now, false) == Result::Busy, "the older mod is busy there");
    Expect(m.Claim("a", 1, "r9", 0, 0, 10, 10, 2, Shape::Arrow, frame, now, true) == Result::Busy, "busy under the overlay");
    Expect(!m.Hit(P(55, 55), frame + 2), "regions lapse after their frames");
    m.Expire(frame + 2);
    Expect(m.RegionCount() == 0, "lapsed regions are dropped");

    // consent: <= 25% of the window and held at the same rect for 500 ms
    m.Claim("a", 1, "big", 0, 0, 1920, 600, 60, Shape::Arrow, frame, now, false);
    m.Claim("a", 1, "btn", 10, 700, 110, 740, 60, Shape::Arrow, frame, now, false);
    now += 600;
    auto click = [&](float px, float py) {
        ModInput::Out d = m.Mouse(WM_LBUTTONDOWN, 0, 0, 0, P(px, py), frame, now, kW, kH);
        ModInput::Out u = m.Mouse(WM_LBUTTONUP, 0, 0, 0, P(px, py), frame, now, kW, kH);
        now += 1000;  // never a double-click
        return u.marks.empty() ? std::string("none") : u.marks[0].second ? "consent" : "gesture";
    };
    ExpectEq(click(50, 50), "gesture", "a region over 25% of the window gives no consent");
    ExpectEq(click(50, 720), "consent", "a small region held for 500 ms gives consent");
    m.Claim("a", 1, "btn", 12, 700, 112, 740, 60, Shape::Arrow, frame, now, false);
    ExpectEq(click(50, 720), "gesture", "a region that just moved gives no consent");
    {
        ModInput::Out d = m.Mouse(WM_LBUTTONDOWN, 0, 0, 0, P(50, 720), frame, now, kW, kH);
        ModInput::Out u = m.Mouse(WM_LBUTTONUP, 0, 0, 0, P(900, 900), frame, now, kW, kH);
        Expect(!u.toGame && u.marks.empty() && u.events.size() == 1, "a release outside is delivered but is not a click");
        now += 1000;
    }
    // double-click
    {
        ModInput::Out d1 = m.Mouse(WM_LBUTTONDOWN, 0, 0, 0, P(50, 720), frame, now, kW, kH);
        m.Mouse(WM_LBUTTONUP, 0, 0, 0, P(50, 720), frame, now, kW, kH);
        ModInput::Out d2 = m.Mouse(WM_LBUTTONDOWN, 0, 0, 0, P(52, 721), frame, now + 200, kW, kH);
        m.Mouse(WM_LBUTTONUP, 0, 0, 0, P(52, 721), frame, now + 200, kW, kH);
        ModInput::Out d3 = m.Mouse(WM_LBUTTONDOWN, 0, 0, 0, P(52, 721), frame, now + 300, kW, kH);
        m.Mouse(WM_LBUTTONUP, 0, 0, 0, P(52, 721), frame, now + 300, kW, kH);
        Expect(!d1.events[0].dbl && d2.events[0].dbl && !d3.events[0].dbl, "second press double, third single again");
        now += 1000;
    }
    // the button tracker
    {
        ModInput::Out d = m.Mouse(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), 0, 0, P(1500, 900), frame, now, kW, kH);
        ModInput::Out u = m.Mouse(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0, 0, P(50, 720), frame, now, kW, kH);
        Expect(d.toGame && u.toGame && u.events.empty(), "a release whose press the game saw always reaches the game");
        ModInput::Out d2 = m.Mouse(WM_RBUTTONDOWN, 0, 0, 0, P(50, 720), frame, now, kW, kH);
        m.Forget("a");
        ModInput::Out u2 = m.Mouse(WM_RBUTTONUP, 0, 0, 0, P(1500, 900), frame, now, kW, kH);
        Expect(!d2.toGame && !u2.toGame && u2.events.empty(), "after unload, a press the mod took still keeps its release");
        m.Claim("a", 1, "btn", 10, 700, 110, 740, 60, Shape::Arrow, frame, now, false);
        ModInput::Out d3 = m.Mouse(WM_LBUTTONDOWN, 0, 0, 0, P(50, 720), frame, now, kW, kH);
        m.OverlaySwallowed(WM_LBUTTONUP, 0);
        ModInput::Out u3 = m.Mouse(WM_LBUTTONUP, 0, 0, 0, P(50, 720), frame, now, kW, kH);
        Expect(!d3.toGame && u3.toGame, "a release the overlay took is no longer the mod's");
        ModInput::Out h = m.Mouse(WM_MOUSEHWHEEL, MAKEWPARAM(0, 120), 0, 0, P(50, 720), frame, now, kW, kH);
        Expect(!h.toGame && h.events.empty(), "horizontal wheel over a region is kept from the game, no event");
        ModInput::Out w = m.Mouse(WM_MOUSEWHEEL, MAKEWPARAM(0, -240), 0, 0, P(50, 720), frame, now, kW, kH);
        Expect(!w.toGame && w.events.size() == 1 && w.events[0].delta == -2, "wheel notches");
        ModInput::Out mv = m.Mouse(WM_MOUSEMOVE, 0, 50, 720, P(50, 720), frame, now, kW, kH);
        Expect(mv.toGame, "moves over a region reach the game");
    }
    // pointer: synthetic releases, consumed click outside, exit grace
    {
        ModInput q;
        ModInput::Out l = q.Mouse(WM_LBUTTONDOWN, 0, 0, 0, P(1, 1), frame, now, kW, kH);
        ModInput::Out r = q.Mouse(WM_RBUTTONDOWN, 0, 0, 0, P(1, 1), frame, now, kW, kH);
        Expect(l.toGame && r.toGame && q.GameHolds(kBtnLeft) && q.GameHolds(kBtnRight), "the game holds left and right");
        std::vector<int> held;
        Expect(q.StartPointer("a", true, 0, false, &held) == Result::Ok && held == std::vector<int>({kBtnLeft, kBtnRight}),
               "pointer start lists both held buttons");
        Expect(q.StartFocus("b", false) == Result::Busy && q.StartPointer("b", true, 0, false, nullptr) == Result::Busy,
               "another mod is busy");
        Expect(q.StartFocus("a", false) == Result::Ok, "the pointer owner may also take focus");
        q.EndFocus();
        Expect(q.Mouse(WM_LBUTTONUP, 0, 0, 0, P(1, 1), frame, now, kW, kH).toGame, "one left release reaches the game");
        Expect(!q.Mouse(WM_LBUTTONUP, 0, 0, 0, P(1, 1), frame, now, kW, kH).toGame, "the second does not");
        Expect(!q.Mouse(WM_INPUT, 0, 0, 0, P(1, 1), frame, now, kW, kH).toGame, "other mouse traffic is kept in pointer mode");
        ModInput::Out o = q.Mouse(WM_MBUTTONDOWN, 0, 0, 0, P(1, 1), frame, now, kW, kH);
        Expect(!o.toGame && o.endPointer && o.events.empty(), "a click outside every region ends it, consumed");
        Expect(std::string(o.endWhy) == "a click outside every region", "the exit names its cause for the log");
        q.EndPointer();
        q.ArmExitGrace(now, 960, 540);
        Expect(!q.Mouse(WM_MBUTTONUP, 0, 0, 0, P(1, 1), frame, now, kW, kH).toGame, "its release stays consumed");
        Expect(q.Mouse(WM_RBUTTONUP, 0, 0, 0, P(1, 1), frame, now, kW, kH).toGame, "the right release still reaches the game");
        Expect(!q.Mouse(WM_MOUSEMOVE, 0, 961, 540, P(1, 1), frame, now + 10, kW, kH).toGame, "grace: a move off-centre is dropped");
        Expect(q.Mouse(WM_MOUSEMOVE, 0, 961, 540, P(1, 1), frame, now + 200, kW, kH).toGame, "the grace times out");
        q.ArmExitGrace(now, 960, 540);
        Expect(q.Mouse(WM_MOUSEMOVE, 0, 960, 540, P(1, 1), frame, now + 10, kW, kH).toGame &&
                   q.Mouse(WM_MOUSEMOVE, 0, 970, 540, P(1, 1), frame, now + 20, kW, kH).toGame,
               "a move at the centre ends the grace");
        // keys in pointer mode and while focused
        q.StartPointer("a", false, 0, false, nullptr);
        Expect(q.Key(WM_KEYDOWN, VK_SPACE, 0x39 << 16, 0, now).toGame, "the keyboard reaches the game in pointer mode");
        ModInput::Out esc = q.Key(WM_KEYDOWN, VK_ESCAPE, 0x01 << 16, 0, now);
        Expect(!esc.toGame && esc.endPointer, "Esc ends pointer mode");
        Expect(std::string(esc.endWhy) == "Esc", "an Esc exit says so");
        q.EndPointer();
        Expect(q.Key(WM_KEYDOWN, VK_ESCAPE, (0x01 << 16) | (1 << 30), 0, now + 999).toGame == false, "the latch eats auto-repeat");
        Expect(q.Key(WM_KEYDOWN, VK_ESCAPE, 0x01 << 16, 0, now + 1001).toGame, "the latch ends");
        q.StartFocus("a", false);
        ModInput::Out rep = q.Key(WM_KEYDOWN, 'V', (0x2F << 16) | (1 << 30), kModCtrl, now);
        Expect(!rep.toGame && !rep.paste && rep.events.size() == 1 && rep.events[0].repeat, "an auto-repeated Ctrl+V does not paste");
        ModInput::Out alt = q.Key(WM_KEYDOWN, 'V', 0x2F << 16, kModCtrl | kModAlt, now);
        Expect(!alt.paste, "AltGr+V (Ctrl+Alt) does not paste");
        ModInput::Out dead = q.Key(WM_DEADCHAR, '^', 0, 0, now);
        Expect(!dead.toGame && dead.events.empty(), "dead characters are kept from the game");
        ModInput::Out lone = q.Key(WM_CHAR, 0xDC00, 0, 0, now);
        Expect(!lone.toGame && lone.events.empty(), "a lone low surrogate is dropped");
        ModInput::Out del = q.Key(WM_CHAR, 0x7F, 0, 0, now);
        Expect(del.events.empty(), "DEL is no character");
    }
}

// ---- the core on a fake Host
namespace host {
bool allowed = true, overlay = false, fg = true, match = true, turn = false, fe = false;
float x = 500, y = 500;
uint64_t frame = 1, now = 100000;
int started = 0, ended = 0;
std::vector<std::string> posted;

void Install() {
    modinput::Host h{};
    h.allowed = [] { return allowed; };
    h.overlayCapturing = [] { return overlay; };
    h.foreground = [] { return fg; };
    h.inMatch = [] { return match; };
    h.frame = [] { return frame; };
    h.nowMs = [] { return now; };
    h.windowSize = [](int* w, int* h2) {
        *w = kW;
        *h2 = kH;
    };
    h.feCursor = [](float* px, float* py) {
        if (!fe) return false;
        *px = x;
        *py = y;
        return true;
    };
    h.osCursor = [](float* px, float* py) {
        *px = x;
        *py = y;
        return true;
    };
    h.modifiers = []() -> uint8_t { return 0; };
    h.clipboard = [](std::wstring*) { return false; };
    h.pointerHook = [] { return true; };
    h.pointerStarted = [](const std::vector<int>&) { ++started; };
    h.pointerEnded = [](const char*, int* cx, int* cy) {
        ++ended;
        *cx = kW / 2;
        *cy = kH / 2;
        return true;
    };
    h.myTurnStarted = [](bool reset) { return !reset && turn; };
    h.post = [](const std::string& mod, const char* name, const std::string& json) {
        posted.push_back(mod + " " + name + " " + json);
    };
    modinput::SetHost(h);
}
}  // namespace host

void TestCore() {
    host::Install();
    Expect(modinput::Pointer("a", true, true, 0) == Result::Ok && modinput::OwnsMouse(), "core: pointer on");
    Expect(modinput::Region("a", 1, "panel", 400, 400, 600, 600, 2, Shape::Hand) == Result::Ok, "core: region");
    ExpectEq(modinput::Hover("a"), "panel", "core: hover with the pointer");
    Expect(modinput::CurrentPointer().shape == Shape::Hand, "core: the drawn cursor takes the region's shape");
    Expect(modinput::CurrentPointer().hotX == 0, "core: no texture, no hotspot");
    Expect(modinput::Pointer("a", true, true, 7, 10, 300) == Result::Ok && modinput::CurrentPointer().hotX == 10 &&
               modinput::CurrentPointer().hotY == 255 && modinput::CurrentPointer().texture == 7,
           "core: the texture hotspot is kept (clamped to 255)");
    Expect(!modinput::RouteMouse(WM_LBUTTONDOWN, 0, 0) && !modinput::RouteMouse(WM_LBUTTONUP, 0, 0), "core: pointer click");
    Expect(host::posted.size() == 2 && host::posted[0].rfind("a melange.input.mouse {\"region\":\"panel\"", 0) == 0,
           "core: owner-only events posted");
    Expect(melange::input::gesture::Recent("a", 1000, false), "core: the click is a gesture");
    // the turn, the overlay, the foreground and AllowModInput end pointer mode in Tick
    host::turn = true;
    modinput::Tick();
    Expect(!modinput::OwnsMouse() && host::ended == 1, "core: a turn of mine ends pointer mode via the zero-delta exit");
    host::turn = false;
    modinput::Pointer("a", true, true, 0);
    host::fg = false;
    modinput::Tick();
    Expect(!modinput::OwnsMouse(), "core: losing the foreground ends pointer mode");
    host::fg = true;
    modinput::Focus("a", true);
    host::allowed = false;
    modinput::Tick();
    Expect(!modinput::OwnsKeyboard(), "core: AllowModInput off ends focus");
    host::allowed = true;
    // expiry happens in Tick
    host::frame += 5;
    modinput::Tick();
    ExpectEq(modinput::Hover("a"), "", "core: lapsed regions are gone after Tick");
    // pointer exit grace through the core
    modinput::Pointer("a", true, true, 0);
    modinput::Pointer("a", false, true, 0);
    Expect(!modinput::RouteMouse(WM_MOUSEMOVE, 0, MAKELPARAM(10, 10)) &&
               modinput::RouteMouse(WM_MOUSEMOVE, 0, MAKELPARAM(kW / 2, kH / 2)),
           "core: after pointer(false) only a move at the centre reaches the game first");
    modinput::Forget("a");
    Expect(!melange::input::gesture::Recent("a", 1000000, false), "core: Forget clears the gestures");
    // the frontend: the game's cursor is drawn under the screen stage, so core draws the region's cursor over a region
    host::fe = true;
    host::x = host::y = 500;
    Expect(!modinput::CurrentPointer().on, "core: no region under the frontend cursor, nothing drawn");
    Expect(modinput::Region("a", 1, "panel", 400, 400, 600, 600, 2, Shape::IBeam) == Result::Ok, "core: fe region");
    {
        const modinput::PointerView v = modinput::CurrentPointer();
        Expect(v.on && v.frontend && v.shape == Shape::IBeam && v.owner == "a" && v.x == 500,
               "core: the frontend cursor over a region is drawn with the region's shape");
    }
    host::x = 700;
    Expect(!modinput::CurrentPointer().on, "core: the frontend cursor outside every region is the game's own");
    host::x = 500;
    host::overlay = true;
    Expect(!modinput::CurrentPointer().on, "core: not while the overlay has the mouse");
    host::overlay = false;
    host::frame += 5;
    modinput::Tick();
    modinput::NoteCover(450, 450, 550, 550);
    Expect(modinput::CurrentPointer().on && modinput::CurrentPointer().shape == Shape::Arrow,
           "core: the frontend cursor over a media picture drawn this frame is drawn on top");
    host::frame += 1;
    Expect(!modinput::CurrentPointer().on, "core: a picture from an earlier frame covers nothing");
    host::fe = false;
    Expect(!modinput::CurrentPointer().on, "core: no frontend cursor (a match), nothing drawn");
}
}  // namespace

int main() {
    TestPure();
    TestCore();
    printf("input_regions_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
