// Overlay input: DirectInput keyboard hooks (vtable slots 9 GetDeviceState, 10 GetDeviceData), a subclassed game
// window that feeds ImGui while capturing, and a SetCursorPos no-op (the game re-centres the cursor every frame).
#include <windows.h>
#include <windowsx.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/mem.h"
#include "render/input_logic.h"
#include "render/internal.h"
#include "render/keytap.h"
#include "melange/overlay.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace melange::render {
bool SehCall(void (*fn)(void*), void* arg, unsigned long* code);  // overlay.cpp
}

namespace {
using melange::render::HotkeyDef;
using melange::render::KeyFilter;
using melange::render::IsKeyMsg;
using melange::render::IsButtonMsg;
using melange::render::ForImGui;
using melange::render::CapturedFromGame;

struct Hotkey {
    int handle;
    uint8_t dik, mods;
    melange::overlay::ActionFn fn;
    void* user;
};
std::mutex g_hkMx;
std::vector<Hotkey> g_hotkeys;
int g_nextHotkey = 1;

std::mutex g_pendMx;
std::vector<int> g_pending;
std::atomic<bool> g_hasPending{false};

std::atomic<uint64_t> g_keysDropped{0}, g_synthetic{0}, g_mouseDropped{0}, g_keyMsgsDropped{0}, g_cursorBlocked{0},
    g_hotkeysFired{0}, g_diPolls{0};
std::atomic<bool> g_diHooked{false}, g_cursorHooked{false}, g_installed{false};

using GetDeviceData_t = HRESULT(WINAPI*)(void*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);
using GetDeviceState_t = HRESULT(WINAPI*)(void*, DWORD, void*);
using CreateDevice_t = HRESULT(WINAPI*)(void*, REFGUID, void**, IUnknown*);
using DI8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
constexpr GUID kGuidSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

struct DevVt {
    GetDeviceData_t data = nullptr;
    GetDeviceState_t state = nullptr;
};
using Release_t = ULONG(WINAPI*)(void*);

std::mutex g_devMx;
std::map<void**, DevVt> g_devVts;             // per device vtable (A and W interfaces differ)
std::map<void**, CreateDevice_t> g_diVts;     // per IDirectInput8 vtable
std::map<void**, Release_t> g_releaseVts;     // per device vtable
std::map<void*, bool> g_isKeyboard;           // device object -> keyboard? erased by HookRelease at refcount 0
DI8Create_t g_origDI8Create = nullptr;

std::mutex g_filterMx;  // the filter and its scratch buffers
KeyFilter g_filter;
std::vector<HotkeyDef> g_defs;
std::vector<int> g_fired;
std::vector<uint8_t> g_released;

// One synthetic key tap: a press record on one poll, the release on a later one, and the key held in the state
// snapshot in between. A press not delivered within kTapStaleMs is dropped; a capture cancels the tap, and since the
// filter records the press as seen by the game, the capture's synthetic release covers it.
constexpr ULONGLONG kTapStaleMs = 250;
std::mutex g_tapMx;  // after g_filterMx when both are held
uint8_t g_tapDik = 0;
int g_tapPhase = 0;   // 0 idle, 1 press due, 2 pressed, 3 release due
ULONGLONG g_tapAsked = 0, g_tapUntil = 0;
DWORD g_tapSeq = 0x40000000;

void ExpireTap(ULONGLONG now) {  // g_tapMx held
    if (g_tapPhase == 1 && now - g_tapAsked > kTapStaleMs) {
        g_tapPhase = 0;
        LOG_INFO("[overlay] synthetic %s tap dropped: not delivered within %u ms", melange::render::DikName(g_tapDik),
                 static_cast<unsigned>(kTapStaleMs));
    }
    if (g_tapPhase == 2 && now >= g_tapUntil) g_tapPhase = 3;
}

// g_filterMx held.
void AppendTap(DIDEVICEOBJECTDATA* buf, DWORD* n, DWORD capacity, bool capturing) {
    std::lock_guard lk(g_tapMx);
    if (capturing) {
        g_tapPhase = 0;
        return;
    }
    ExpireTap(GetTickCount64());
    if ((g_tapPhase != 1 && g_tapPhase != 3) || *n >= capacity) return;
    DIDEVICEOBJECTDATA& d = buf[*n];
    d = {};
    d.dwOfs = g_tapDik;
    d.dwData = g_tapPhase == 1 ? 0x80 : 0;
    d.dwTimeStamp = GetTickCount();
    d.dwSequence = ++g_tapSeq;
    ++*n;
    g_filter.NoteGame(g_tapDik, g_tapPhase == 1);
    g_tapPhase = g_tapPhase == 1 ? 2 : 0;
}

void HoldTap(uint8_t* state, DWORD cb) {
    std::lock_guard lk(g_tapMx);
    ExpireTap(GetTickCount64());
    if ((g_tapPhase == 1 || g_tapPhase == 2) && g_tapDik < cb) state[g_tapDik] |= 0x80;
}

bool Lookup(void* self, DevVt* vt) {
    std::lock_guard lk(g_devMx);
    auto v = g_devVts.find(*static_cast<void***>(self));
    if (v == g_devVts.end()) return false;
    *vt = v->second;
    auto k = g_isKeyboard.find(self);
    return k != g_isKeyboard.end() && k->second;
}

HRESULT WINAPI HookGetDeviceData(void* self, DWORD cb, DIDEVICEOBJECTDATA* rgdod, DWORD* inOut, DWORD flags) {
    DevVt o;
    bool kb = Lookup(self, &o);
    if (!o.data) return DIERR_GENERIC;
    DWORD capacity = inOut ? *inOut : 0;
    HRESULT hr = o.data(self, cb, rgdod, inOut, flags);
    if (!kb) return hr;
    if (FAILED(hr)) {
        if (hr == DIERR_NOTACQUIRED || hr == DIERR_INPUTLOST) {
            std::lock_guard lk(g_filterMx);
            g_filter.ResetDevice();  // focus lost: whatever was held is gone for the game as well
        }
        return hr;
    }
    if (!rgdod || !inOut || cb != sizeof(DIDEVICEOBJECTDATA) || (flags & DIGDD_PEEK)) return hr;
    ++g_diPolls;
    const bool capturing = melange::overlay::Capturing();
    std::vector<int> fired;
    std::vector<uint8_t> released;
    {
        std::lock_guard lk(g_filterMx);
        g_defs.clear();
        {
            std::lock_guard hk(g_hkMx);
            for (const Hotkey& h : g_hotkeys) g_defs.push_back({h.handle, h.dik, h.mods});
        }
        g_fired.clear();
        g_released.clear();
        uint64_t droppedBefore = g_filter.Dropped();
        *inOut = g_filter.Process(rgdod, *inOut, capacity, capturing, g_defs.data(), g_defs.size(), &g_fired,
                                  &g_released, GetTickCount());
        g_keysDropped += g_filter.Dropped() - droppedBefore;
        if (!g_fired.empty()) fired.swap(g_fired);
        if (!g_released.empty()) released.swap(g_released);
        AppendTap(rgdod, inOut, capacity, capturing);
    }
    if (!released.empty()) {
        g_synthetic += released.size();
        std::string names;
        for (uint8_t d : released) {
            if (!names.empty()) names += ' ';
            names += melange::render::DikName(d);
        }
        LOG_INFO("[overlay] capture on: synthetic key release sent to the game for %s", names.c_str());
    }
    if (!fired.empty()) {
        g_hotkeysFired += fired.size();
        std::lock_guard lk(g_pendMx);
        g_pending.insert(g_pending.end(), fired.begin(), fired.end());
        g_hasPending = true;
    }
    return hr;
}

HRESULT WINAPI HookGetDeviceState(void* self, DWORD cb, void* data) {
    DevVt o;
    bool kb = Lookup(self, &o);
    if (!o.state) return DIERR_GENERIC;
    HRESULT hr = o.state(self, cb, data);
    if (kb && SUCCEEDED(hr) && data) {
        std::lock_guard lk(g_filterMx);
        g_filter.FilterState(static_cast<uint8_t*>(data), cb, melange::overlay::Capturing());
        if (!melange::overlay::Capturing()) HoldTap(static_cast<uint8_t*>(data), cb);
    }
    return hr;
}

// While unfocused the engine recreates its DirectInput devices every ~250 ms, so drop each device's g_isKeyboard
// entry when its refcount reaches 0. Hooked once per vtable.
ULONG WINAPI HookRelease(void* self) {
    Release_t orig;
    {
        std::lock_guard lk(g_devMx);
        auto it = g_releaseVts.find(*static_cast<void***>(self));
        orig = it != g_releaseVts.end() ? it->second : nullptr;
    }
    ULONG rc = orig ? orig(self) : 0;
    if (rc == 0) {
        std::lock_guard lk(g_devMx);
        g_isKeyboard.erase(self);
    }
    return rc;
}

HRESULT WINAPI HookCreateDevice(void* self, REFGUID guid, void** out, IUnknown* outer) {
    CreateDevice_t orig;
    {
        std::lock_guard lk(g_devMx);
        orig = g_diVts[*static_cast<void***>(self)];
    }
    if (!orig) return DIERR_GENERIC;
    HRESULT hr = orig(self, guid, out, outer);
    if (FAILED(hr) || !out || !*out) return hr;
    const bool kb = IsEqualGUID(guid, kGuidSysKeyboard) != 0;
    void** vt = *static_cast<void***>(*out);
    bool hookedNow = false;
    DevVt o;
    {
        std::lock_guard lk(g_devMx);
        g_isKeyboard[*out] = kb;
        if (!g_releaseVts.count(vt)) {
            Release_t orel = nullptr;
            melange::mem::HookVTable(*out, 2, reinterpret_cast<void*>(&HookRelease), reinterpret_cast<void**>(&orel));
            g_releaseVts[vt] = orel;
        }
        if (kb && !g_devVts.count(vt)) {
            melange::mem::HookVTable(*out, 10, reinterpret_cast<void*>(&HookGetDeviceData), reinterpret_cast<void**>(&o.data));
            melange::mem::HookVTable(*out, 9, reinterpret_cast<void*>(&HookGetDeviceState), reinterpret_cast<void**>(&o.state));
            g_devVts[vt] = o;
            hookedNow = true;
        }
    }
    if (kb) {
        std::lock_guard lk(g_filterMx);
        g_filter.ResetDevice();
    }
    if (hookedNow) {
        g_diHooked = true;
        LOG_INFO("[overlay] DI keyboard %p vtable %p hooked (chained GetDeviceData=%p)", *out, static_cast<void*>(vt),
                reinterpret_cast<void*>(o.data));
    }
    return hr;
}

HRESULT WINAPI HookDI8Create(HINSTANCE inst, DWORD ver, REFIID riid, void** out, IUnknown* outer) {
    HRESULT hr = g_origDI8Create(inst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        std::lock_guard lk(g_devMx);
        void** vt = *static_cast<void***>(*out);
        if (!g_diVts.count(vt)) {
            CreateDevice_t o = nullptr;
            melange::mem::HookVTable(*out, 3, reinterpret_cast<void*>(&HookCreateDevice), reinterpret_cast<void**>(&o));
            g_diVts[vt] = o;
        }
    }
    return hr;
}

using SetCursorPos_t = BOOL(WINAPI*)(int, int);
SetCursorPos_t g_origSetCursorPos = nullptr;

BOOL WINAPI HookSetCursorPos(int x, int y) {
    if (melange::overlay::Capturing()) {
        ++g_cursorBlocked;
        return TRUE;
    }
    return g_origSetCursorPos(x, y);
}

struct Sub {
    HWND hwnd;
    WNDPROC orig;
    bool unicode;
};
std::vector<Sub> g_subs;  // main thread only (subclassing and the window procedure both run there)
HWND g_subHwnd = nullptr;
std::atomic<bool> g_imguiReady{false};
melange::render::AltReleaseGuard g_altGuard;  // main thread only, like the window procedure

// A key message whose scan code (+ modifiers) is a registered hotkey.
bool IsHotkeyKeyMsg(LPARAM lp) {
    uint8_t scan = static_cast<uint8_t>((lp >> 16) & 0xFF);
    bool ext = ((lp >> 24) & 1) != 0;
    uint8_t dik = static_cast<uint8_t>((scan & 0x7F) | (ext ? 0x80 : 0));
    uint8_t mods = 0;
    if (GetKeyState(VK_CONTROL) < 0) mods |= melange::render::kModCtrl;
    if (GetKeyState(VK_SHIFT) < 0) mods |= melange::render::kModShift;
    if (GetKeyState(VK_MENU) < 0) mods |= melange::render::kModAlt;
    std::lock_guard lk(g_hkMx);
    for (const Hotkey& h : g_hotkeys)
        if (h.dik == dik && h.mods == mods) return true;
    return false;
}

LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC orig = nullptr;
    bool unicode = true;
    for (const Sub& s : g_subs)
        if (s.hwnd == h) {
            orig = s.orig;
            unicode = s.unicode;
            break;
        }
    if (!orig) return unicode ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);

    const bool hotkeyMsg = IsKeyMsg(msg) && IsHotkeyKeyMsg(lp);
    if (g_altGuard.Drop(msg, wp, lp, hotkeyMsg) || hotkeyMsg) {
        ++g_keyMsgsDropped;
        return 0;
    }
    const bool capturing = melange::overlay::Capturing();
    // ImGui readiness only gates whether the backend gets the message, never whether the game does: a message the
    // overlay is capturing must never reach the game just because the backend is mid-reinit, or a box could lose a
    // keystroke to the game instead of to ImGui (and the game would see input the overlay was meant to hide).
    if (capturing && g_imguiReady.load(std::memory_order_relaxed) && ForImGui(msg)) {
        // Posted clicks may not match the real cursor position: take the position from the click itself.
        if (IsButtonMsg(msg))
            ImGui::GetIO().AddMousePosEvent(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
    }
    if (CapturedFromGame(msg, capturing)) {
        if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR)
            ++g_keyMsgsDropped;
        else
            ++g_mouseDropped;
        return 0;
    }
    return unicode ? CallWindowProcW(orig, h, msg, wp, lp) : CallWindowProcA(orig, h, msg, wp, lp);
}
}  // namespace

namespace melange::render {
bool InstallInput() {
    if (g_installed.exchange(true)) return true;
    bool di = melange::mem::HookIAT("DINPUT8.dll", "DirectInput8Create", reinterpret_cast<void*>(&HookDI8Create),
                               reinterpret_cast<void**>(&g_origDI8Create));
    bool cur = melange::mem::HookIAT("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&HookSetCursorPos),
                                reinterpret_cast<void**>(&g_origSetCursorPos));
    g_cursorHooked = cur;
    if (!di) LOG_WARN("[overlay] DINPUT8!DirectInput8Create import not hooked: hotkeys and keyboard capture will not work");
    if (!cur) LOG_WARN("[overlay] USER32!SetCursorPos import not hooked: the game will keep re-centring the cursor");
    return di;
}

bool InputInstalled() { return g_installed.load(); }

bool HotkeysPending() { return g_hasPending.load(std::memory_order_relaxed); }

void RunPendingHotkeys() {
    std::vector<int> run;
    {
        std::lock_guard lk(g_pendMx);
        run.swap(g_pending);
        g_hasPending = false;
    }
    for (int handle : run) {
        Hotkey h{};
        {
            std::lock_guard lk(g_hkMx);
            for (const Hotkey& k : g_hotkeys)
                if (k.handle == handle) h = k;
        }
        if (!h.fn) continue;
        LOG_INFO("[overlay] hotkey %s", HotkeyLabel(h.dik, h.mods).c_str());
        unsigned long code = 0;
        if (!SehCall(h.fn, h.user, &code))
            LOG_ERROR("[overlay] hotkey %s action raised exception 0x%08lx", HotkeyLabel(h.dik, h.mods).c_str(), code);
    }
}

void SubclassGameWindow(HWND hwnd) {
    if (!hwnd || hwnd == g_subHwnd || !IsWindow(hwnd)) return;
    if (g_subHwnd) {
        // A new game window: unhook the old one if it still exists and nobody chained on top of us.
        for (auto it = g_subs.begin(); it != g_subs.end(); ++it) {
            if (it->hwnd != g_subHwnd) continue;
            if (IsWindow(it->hwnd) &&
                reinterpret_cast<WNDPROC>(GetWindowLongPtrW(it->hwnd, GWLP_WNDPROC)) == &OverlayWndProc) {
                if (it->unicode)
                    SetWindowLongPtrW(it->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->orig));
                else
                    SetWindowLongPtrA(it->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->orig));
                g_subs.erase(it);
            } else if (!IsWindow(it->hwnd)) {
                g_subs.erase(it);
            }
            break;
        }
    }
    Sub s{hwnd, nullptr, IsWindowUnicode(hwnd) != FALSE};
    g_subs.push_back(s);  // before SetWindowLongPtr: messages may arrive immediately
    auto proc = reinterpret_cast<LONG_PTR>(&OverlayWndProc);
    LONG_PTR old = s.unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, proc) : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, proc);
    g_subs.back().orig = reinterpret_cast<WNDPROC>(old);
    g_subHwnd = hwnd;
    LOG_INFO("[overlay] window %p subclassed (orig wndproc %p, %s)", static_cast<void*>(hwnd), reinterpret_cast<void*>(old),
            s.unicode ? "unicode" : "ansi");
}

void SetImGuiInputReady(bool ready) { g_imguiReady = ready; }

bool TapKey(uint8_t dik, int holdMs) {
    if (!g_diHooked || !dik) return false;
    std::lock_guard lk(g_tapMx);
    if (g_tapPhase != 0) return false;
    g_tapDik = dik;
    g_tapPhase = 1;
    g_tapAsked = GetTickCount64();
    g_tapUntil = g_tapAsked + static_cast<ULONGLONG>(holdMs < 0 ? 0 : holdMs);
    return true;
}

void CancelTap() {
    std::lock_guard lk(g_tapMx);
    if (g_tapPhase == 1) g_tapPhase = 0;
    else if (g_tapPhase == 2) g_tapPhase = 3;
}

InputStats GetInputStats() {
    InputStats s;
    s.keysDropped = g_keysDropped;
    s.syntheticReleases = g_synthetic;
    s.mouseMsgsDropped = g_mouseDropped;
    s.keyMsgsDropped = g_keyMsgsDropped;
    s.cursorPosBlocked = g_cursorBlocked;
    s.hotkeysFired = g_hotkeysFired;
    s.diPolls = g_diPolls;
    s.diHooked = g_diHooked;
    s.cursorHooked = g_cursorHooked;
    s.subclassed = g_subHwnd != nullptr;
    return s;
}

std::vector<HotkeyInfo> ListHotkeys() {
    std::vector<HotkeyInfo> out;
    std::lock_guard lk(g_hkMx);
    for (const Hotkey& h : g_hotkeys) out.push_back({h.handle, HotkeyLabel(h.dik, h.mods)});
    return out;
}
}  // namespace melange::render

namespace melange::overlay {
int AddHotkey(uint8_t dik, uint8_t mods, ActionFn fn, void* user) {
    if (!dik || !fn || (mods & ~(kCtrl | kShift | kAlt))) return 0;
    std::lock_guard lk(g_hkMx);
    for (const Hotkey& h : g_hotkeys)
        if (h.dik == dik && h.mods == mods)
            LOG_WARN("[overlay] hotkey %s registered twice; both actions will run", melange::render::HotkeyLabel(dik, mods).c_str());
    int handle = g_nextHotkey++;
    g_hotkeys.push_back({handle, dik, mods, fn, user});
    return handle;
}

bool ParseHotkey(const char* text, uint8_t* dik, uint8_t* mods) { return melange::render::ParseHotkeyText(text, dik, mods); }
}  // namespace melange::overlay
