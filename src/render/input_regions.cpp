// Mod input in the game (input_regions.h): the Host behind input_regions_core.cpp (cursors, window, match and turn
// signals, the clipboard, owner-only events) and the pointer-mode cursor drawn at present time. Local input only:
// nothing here touches the simulation or sends anything.
#include <windows.h>

#include <GL/gl.h>

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include "core/events.h"
#include "core/log.h"
#include "media/config_keys.h"
#include "net/net.h"
#include "render/input_logic.h"
#include "render/input_regions.h"
#include "render/internal.h"
#include "melange/gamestate.h"
#include "melange/lua.h"
#include "melange/overlay.h"
#include "melange/render.h"
#include "melange/sim.h"

namespace melange::render {
namespace {
namespace mi = modinput;

bool g_allowed = true;  // [Input] AllowModInput, read once at install

HWND Window() { return static_cast<HWND>(events::GameWindow()); }

bool Allowed() { return g_allowed && SubclassedWindow() != nullptr; }
bool OverlayCapturing() { return overlay::Capturing(); }

bool Foreground() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == Window()) return true;
    DWORD pid = 0;
    return GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

bool InMatch() { return sim::InMatch(); }
uint64_t Frame() { return events::FrameCount(); }
uint64_t NowMs() { return GetTickCount64(); }

void ClientSize(int* w, int* h) {
    RECT r{};
    if (HWND hwnd = Window()) GetClientRect(hwnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

bool FeCursor(float* x, float* y) {
    int w = 0, h = 0;
    ClientSize(&w, &h);
    return FrontendCursor(w, h, x, y);
}

bool OsCursor(float* x, float* y) {
    HWND hwnd = Window();
    POINT p;
    RECT r;
    if (!hwnd || !GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &r) || r.right <= 0 || r.bottom <= 0)
        return false;
    *x = static_cast<float>(p.x < 0 ? 0 : p.x >= r.right ? r.right - 1 : p.x);
    *y = static_cast<float>(p.y < 0 ? 0 : p.y >= r.bottom ? r.bottom - 1 : p.y);
    return true;
}

uint8_t Modifiers() {
    uint8_t m = 0;
    if (GetKeyState(VK_CONTROL) < 0) m |= kModCtrl;
    if (GetKeyState(VK_SHIFT) < 0) m |= kModShift;
    if (GetKeyState(VK_MENU) < 0) m |= kModAlt;
    return m;
}

// Read only on an explicit Ctrl+V into a focused mod box (there is no general clipboard read).
bool Clipboard(std::wstring* out) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(Window())) return false;
    bool ok = false;
    if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(d))) {
            const size_t cap = GlobalSize(d) / sizeof(wchar_t);
            size_t n = 0;
            while (n < cap && n < mi::kPasteMaxUnits && p[n]) ++n;
            out->assign(p, n);
            ok = true;
            GlobalUnlock(d);
        }
    }
    CloseClipboard();
    return ok;
}

bool PointerHook() {
    static bool logged = false;
    if (InstallWarpHook()) return true;
    if (!logged) {
        logged = true;
        LOG_WARN("[input] pointer mode unavailable: the cursor warp could not be hooked (unknown build?)");
    }
    return false;
}

void PointerStarted(const std::vector<int>& held) {
    PostButtonUps(held);
    LOG_INFO("[input] pointer mode on for %s (%u held button(s) released for the game)",
             mi::CurrentPointer().owner.c_str(), static_cast<unsigned>(held.size()));
}

bool PointerEnded(const char* why, int* cx, int* cy) {
    const bool ok = PointerExit(cx, cy);
    const bool away = !ok && GetForegroundWindow() != static_cast<HWND>(melange::events::GameWindow());
    LOG_INFO("[input] pointer mode off: %s%s", why && *why ? why : "ended",
             ok ? "" : away ? " (unfocused: the cursor is left alone until the game is back)"
                            : " (no warp centre: the game may see one mouse jump)");
    return ok;
}

// A turn of this machine's player started: online when the NetPlayer whose turn it is becomes local (NetPlayer
// +0x4e, unknown between turns); offline when a turn starts for a local human team (local and not CPU).
struct TurnWatch {
    wum::Tri last = wum::Tri::Unknown;
    uint32_t turns = 0;
    int pending = 0;  // frames left to learn the new turn's team
} g_turn;

bool MyTurnStarted(bool reset) {
    const bool online = wum::Phase() != wum::SessionPhase::None;
    gamestate::Snapshot s{};
    const bool snap = !online && gamestate::Read(&s);
    if (reset) {
        g_turn = TurnWatch{};
        g_turn.last = online ? wum::CurrentPlayerIsLocal() : wum::Tri::Unknown;
        if (snap) g_turn.turns = s.match.turnsStarted;
        return false;
    }
    if (online) {
        const wum::Tri t = wum::CurrentPlayerIsLocal();
        const bool started = t == wum::Tri::Yes && g_turn.last != wum::Tri::Yes;
        g_turn.last = t;
        return started;
    }
    if (!snap) return false;
    if (s.match.turnsStarted != g_turn.turns) {
        g_turn.turns = s.match.turnsStarted;
        g_turn.pending = 120;
    }
    if (g_turn.pending <= 0 || s.match.currentTeam < 0) {
        if (g_turn.pending > 0) --g_turn.pending;
        return false;
    }
    g_turn.pending = 0;
    for (int i = 0; i < s.teamCount && i < 4; ++i)
        if (s.teams[i].slot == s.match.currentTeam) return s.teams[i].local && !s.teams[i].ai;
    return false;
}

void Post(const std::string& mod, const char* name, const std::string& json) {
    lua::PostEventTo(mod.c_str(), name, json.c_str());
}

// ---- the pointer-mode cursor
// Outline polygons in 1/16 cursor units, hotspot at (0, 0). The arrow is star-shaped around its tip, so a triangle
// fan from the tip fills it.
constexpr float kArrow[][2] = {{0, 0}, {0, 17}, {4, 13}, {7, 20}, {9, 19}, {6, 12}, {12, 12}};
constexpr float kFinger[][2] = {{-2, 0}, {2, 0}, {2, 8}, {-2, 8}};
constexpr float kPalm[][2] = {{-5, 7}, {8, 7}, {8, 18}, {-5, 18}};

void Poly(const float (*pts)[2], int n, float x, float y, float s, bool fan) {
    glColor4f(1, 1, 1, 1);
    glBegin(fan ? GL_TRIANGLE_FAN : GL_QUADS);
    for (int i = 0; i < n; ++i) glVertex2f(x + pts[i][0] * s, y + pts[i][1] * s);
    glEnd();
    glColor4f(0, 0, 0, 1);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < n; ++i) glVertex2f(x + pts[i][0] * s, y + pts[i][1] * s);
    glEnd();
}

void DrawShape(mi::Shape shape, float x, float y, float s) {
    glDisable(GL_TEXTURE_2D);
    glLineWidth(s < 1.5f ? 1.0f : 2.0f);
    if (shape == mi::Shape::Hand) {
        Poly(kPalm, 4, x, y, s, false);
        Poly(kFinger, 4, x, y, s, false);
    } else if (shape == mi::Shape::IBeam) {
        const float h = 9 * s, w = 3 * s;
        for (int pass = 0; pass < 2; ++pass) {
            glLineWidth(pass == 0 ? 3.0f * (s < 1.5f ? 1.0f : 2.0f) : (s < 1.5f ? 1.0f : 2.0f));
            glColor4f(pass == 0 ? 0.0f : 1.0f, pass == 0 ? 0.0f : 1.0f, pass == 0 ? 0.0f : 1.0f, 1);
            glBegin(GL_LINES);
            glVertex2f(x, y - h);
            glVertex2f(x, y + h);
            glVertex2f(x - w, y - h);
            glVertex2f(x + w, y - h);
            glVertex2f(x - w, y + h);
            glVertex2f(x + w, y + h);
            glEnd();
        }
    } else {
        Poly(kArrow, 7, x, y, s, true);
    }
}

// The owner's texture at its own size, its hotspot pixel (opts.hot, default the top-left corner) on the pointer.
void DrawTexture(unsigned tex, float x, float y) {
    GLint tw = 0, th = 0;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if (tw <= 0 || th <= 0 || tw > 256 || th > 256) tw = th = 32;  // a cursor, not a picture
    const float w = static_cast<float>(tw), h = static_cast<float>(th);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2f(x, y);
    glTexCoord2f(1, 0);
    glVertex2f(x + w, y);
    glTexCoord2f(1, 1);
    glVertex2f(x + w, y + h);
    glTexCoord2f(0, 1);
    glVertex2f(x, y + h);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}
}  // namespace

void InstallModInput() {
    static bool done = false;
    if (done) return;
    done = true;
    g_allowed = media::cfg::AllowModInput();
    if (!g_allowed) LOG_INFO("[input] [Input] AllowModInput=0: mod regions, pointer mode and focus are off");
    mi::Host h{};
    h.allowed = &Allowed;
    h.overlayCapturing = &OverlayCapturing;
    h.foreground = &Foreground;
    h.inMatch = &InMatch;
    h.frame = &Frame;
    h.nowMs = &NowMs;
    h.windowSize = &ClientSize;
    h.feCursor = &FeCursor;
    h.osCursor = &OsCursor;
    h.modifiers = &Modifiers;
    h.clipboard = &Clipboard;
    h.pointerHook = &PointerHook;
    h.pointerStarted = &PointerStarted;
    h.pointerEnded = &PointerEnded;
    h.myTurnStarted = &MyTurnStarted;
    h.post = &Post;
    mi::SetHost(h);
    RegisterFrontendCursorVerb();
    events::Subscribe(events::Event::Frame, [] { mi::Tick(); });
}

void DrawModPointer() {
    const mi::PointerView v = mi::CurrentPointer();
    if (!v.on) return;
    int w = 0, h = 0;
    ClientSize(&w, &h);
    if (w <= 0 || h <= 0) return;
    const uint32_t token = PushState();
    if (!token) return;
    GLint vp[4] = {};
    glGetIntegerv(GL_VIEWPORT, vp);
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float s = static_cast<float>(h) / 720.0f;  // 1 at 720p, 1.5 at 1080p
    s = s < 1 ? 1 : s > 3 ? 3 : s;
    // Window px are pixel centres; +0.5 keeps the 1 px outline crisp.
    const float x = std::floor(v.x) + 0.5f, y = std::floor(v.y) + 0.5f;
    if (v.texture && glIsTexture(v.texture))
        DrawTexture(v.texture, x - 0.5f - std::floor(v.hotX), y - 0.5f - std::floor(v.hotY));
    else
        DrawShape(v.shape, x, y, s);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    PopState(token);
}
}  // namespace melange::render
