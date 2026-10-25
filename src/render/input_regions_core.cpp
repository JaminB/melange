// Mod input state and routing (input_regions.h): one ModInput for the process, driven through a Host so the
// self-tests run the same code without the game. Main thread only.
#include <windows.h>
#include <windowsx.h>

#include "input/gesture.h"
#include "render/input_logic.h"
#include "render/input_regions.h"

namespace melange::render::modinput {
namespace {
Host g_host{};
bool g_haveHost = false;
ModInput g_in;

bool Ready() { return g_haveHost && g_host.allowed(); }

ModInput::Point Sample() {
    ModInput::Point p;
    if (!g_haveHost) return p;
    p.valid = !g_in.PointerOwner().empty() ? g_host.osCursor(&p.x, &p.y) : g_host.feCursor(&p.x, &p.y);
    return p;
}

void EndPointerNow() {
    if (g_in.PointerOwner().empty()) return;
    g_in.EndPointer();
    int cx = 0, cy = 0;
    if (g_host.pointerEnded(&cx, &cy)) g_in.ArmExitGrace(g_host.nowMs(), cx, cy);
}

void Apply(ModInput::Out& o) {
    for (const ModEvent& e : o.events) g_host.post(e.mod, ModEventName(e.kind), ModEventJson(e));
    for (const auto& [mod, consent] : o.marks) input::gesture::Mark(mod.c_str(), consent);
    if (o.endPointer) EndPointerNow();
    if (o.endFocus) g_in.EndFocus();
}

void EndAll() {
    EndPointerNow();
    g_in.EndFocus();
}
}  // namespace

void SetHost(const Host& host) {
    g_host = host;
    g_haveHost = true;
    g_in.SetDoubleClick(GetDoubleClickTime(), GetSystemMetrics(SM_CXDOUBLECLK) / 2, GetSystemMetrics(SM_CYDOUBLECLK) / 2);
}

bool Cursor(const std::string& mod, float* x, float* y, const char** source) {
    if (!Ready()) return false;
    if (!g_in.PointerOwner().empty()) {
        if (g_in.PointerOwner() != mod || !g_host.osCursor(x, y)) return false;
        *source = "pointer";
        return true;
    }
    if (!g_host.feCursor(x, y)) return false;
    *source = "frontend";
    return true;
}

Result Region(const std::string& mod, int order, const std::string& id, float x0, float y0, float x1, float y1,
              int frames, Shape shape) {
    if (!Ready()) return Result::Unavailable;
    return g_in.Claim(mod, order, id, x0, y0, x1, y1, frames, shape, g_host.frame(), g_host.nowMs(),
                      g_host.overlayCapturing());
}

std::string Hover(const std::string& mod) {
    if (!Ready()) return {};
    if (!g_in.PointerOwner().empty() && g_in.PointerOwner() != mod) return {};
    const ModInput::Region* r = g_in.Hit(Sample(), g_host.frame(), &mod);
    return r ? r->id : std::string();
}

Result Pointer(const std::string& mod, bool on, bool clickOutside, unsigned texture) {
    if (!Ready()) return Result::Unavailable;
    if (!on) {
        if (g_in.PointerOwner() == mod) EndPointerNow();
        return Result::Ok;
    }
    if (!g_host.inMatch()) return Result::Unavailable;
    if (g_host.overlayCapturing()) return Result::Busy;
    const bool fresh = g_in.PointerOwner().empty();
    if (fresh && !g_host.pointerHook()) return Result::Unavailable;
    std::vector<int> held;
    const Result r = g_in.StartPointer(mod, clickOutside, texture, false, &held);
    if (r == Result::Ok && fresh) {
        g_host.pointerStarted(held);
        g_host.myTurnStarted(true);  // only a turn that starts from now on ends it
    }
    return r;
}

bool PointerActive(const std::string& mod) { return Ready() && !mod.empty() && g_in.PointerOwner() == mod; }

Result Focus(const std::string& mod, bool on) {
    if (!Ready()) return Result::Unavailable;
    if (!on) {
        if (g_in.FocusOwner() == mod) g_in.EndFocus();
        return Result::Ok;
    }
    return g_in.StartFocus(mod, g_host.overlayCapturing());
}

void Forget(const std::string& mod) {
    if (g_in.PointerOwner() == mod) EndPointerNow();
    g_in.Forget(mod);
    input::gesture::Clear(mod.c_str());
}

bool OwnsMouse() { return !g_in.PointerOwner().empty(); }
bool OwnsKeyboard() { return !g_in.FocusOwner().empty(); }
bool SuppressBareFKeys() { return OwnsKeyboard(); }

bool EatEsc() {
    if (!g_haveHost) return false;
    return (OwnsMouse() && !OwnsKeyboard()) || g_in.EscLatched(g_host.nowMs());
}

void EscFromDirectInput() {
    // With focus held the window's WM_KEYDOWN delivers Esc to the mod and ends focus; DirectInput only drops it.
    if (!g_haveHost || OwnsKeyboard()) return;
    if (OwnsMouse()) {
        g_in.LatchEsc(g_host.nowMs());
        EndPointerNow();
    }
}

bool RouteMouse(unsigned msg, uintptr_t wp, intptr_t lp) {
    if (!g_haveHost) return true;
    // Moves are hit-tested by nobody: skip reading the frontend cursor for them.
    const ModInput::Point c = msg == WM_MOUSEMOVE ? ModInput::Point{} : Sample();
    int w = 0, h = 0;
    g_host.windowSize(&w, &h);
    ModInput::Out o = g_in.Mouse(msg, static_cast<WPARAM>(wp), GET_X_LPARAM(lp), GET_Y_LPARAM(lp), c, g_host.frame(),
                                 g_host.nowMs(), w, h);
    Apply(o);
    return o.toGame;
}

bool RouteKey(unsigned msg, uintptr_t wp, intptr_t lp) {
    if (!g_haveHost) return true;
    const uint64_t now = g_host.nowMs();
    if (!OwnsKeyboard() && !OwnsMouse() && !g_in.EscLatched(now)) return true;
    ModInput::Out o = g_in.Key(msg, static_cast<WPARAM>(wp), static_cast<LPARAM>(lp), g_host.modifiers(), now);
    if (o.paste) {
        std::wstring text;
        if (g_host.clipboard(&text)) {
            ModEvent e;
            e.kind = ModEvent::Kind::Paste;
            e.mod = g_in.FocusOwner();
            e.text = SanitizePaste(text);
            if (!e.text.empty()) o.events.push_back(std::move(e));
        }
    }
    Apply(o);
    return o.toGame;
}

void NoteSwallowedByOverlay(unsigned msg, uintptr_t wp) { g_in.OverlaySwallowed(msg, static_cast<WPARAM>(wp)); }

void FocusLost() {
    g_in.LoseFocus();
    if (g_haveHost) EndAll();
}

void Tick() {
    if (!g_haveHost) return;
    g_in.Expire(g_host.frame());
    if (!OwnsMouse() && !OwnsKeyboard()) return;
    if (!g_host.allowed() || g_host.overlayCapturing() || !g_host.foreground()) {
        EndAll();
        return;
    }
    if (OwnsMouse() && (!g_host.inMatch() || g_host.myTurnStarted(false))) EndPointerNow();
}

PointerView CurrentPointer() {
    PointerView v;
    if (!g_haveHost || g_in.PointerOwner().empty()) return v;
    v.on = true;
    v.owner = g_in.PointerOwner();
    v.texture = g_in.PointerTexture();
    ModInput::Point p;
    p.valid = g_host.osCursor(&p.x, &p.y);
    v.x = p.x;
    v.y = p.y;
    if (const ModInput::Region* r = g_in.Hit(p, g_host.frame(), &v.owner)) v.shape = r->shape;
    return v;
}

std::string FocusOwner() { return g_in.FocusOwner(); }
}  // namespace melange::render::modinput
