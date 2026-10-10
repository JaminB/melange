// Controls: mouse look/aim options for wum.input and the Raw Input mouse smoothing ([Controls] SmoothMouse).
//
// LOCKSTEP RULE. Everything here happens on the SENDING side, before the game builds (and replicates) its own mouse
// messages: the invert flags and the numbers handed to the mouse-message poster 0x505990 are changed there, so peers
// and Wormsign recordings receive the final values like any other input. Nothing reads or writes TWEAK data, camera
// state, SetCamera, the ActiveObject list, the save file or anything run on the receiving side, and no engine message
// is posted from here.
#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "input/controls.h"
#include "melange/bus.h"
#include "melange/overlay.h"
#include "melange/testcmd.h"
#include "render/input_logic.h"
#include "render/internal.h"

namespace melange::controls {
namespace {
constexpr uintptr_t kSvcPtr = 0x95B2A4;        // InputTranslationService instance pointer
constexpr uintptr_t kFlush = 0x505c70;         // thiscall(svc): posts the accumulated mouse motion
constexpr uintptr_t kPost = 0x505990;          // thiscall(svc, msgId, x, y), ret 0xC: the mouse-message poster
constexpr uintptr_t kTick = 0x5060e0;          // per-tick service update; svc is the first stack argument
constexpr uintptr_t kCameraMgrPtr = 0x95c370;  // CameraManager*
constexpr uintptr_t kBlimpView = 0x51d8f0;     // thiscall(CameraManager*) -> bool, read-only
constexpr int kGroupCount = 19;
constexpr uintptr_t kGroupStride = 0x30;

const char* const kGroupNames[kGroupCount] = {
    "Menu",           "InGame",        "WormAiming",    "WormFirstPersonAiming", "WormMoving",
    "WormRoping",     "UtilityGirder", "Flying",        "CameraSelect",          "Fire",
    "UtilityFire",    "CrateChuteRelease", "Spectator", "NetworkSpectator",       "AttractMode",
    "ControllerRemoved", "ManualCam",  "EFMVMovie",     "XboxSignIn"};

enum Kind : int { kNone = 0, kCamera = 1, kSpectate = 2, kAim = 3 };

bool g_installed = false, g_smoothCfg = true, g_probe = false;
Options g_opt;
const void* g_owner = nullptr;
std::vector<SafetyHookMid> g_hooks;

template <class T>
bool Rd(uintptr_t a, T& v) {
    return melange::mem::SafeRead(a, &v, sizeof v);
}

uintptr_t SvcAddr() {
    uint32_t p = 0;
    return Rd(kSvcPtr, p) ? p : 0;
}

// ---------------------------------------------------------------- control groups and mappings
struct Layout {
    uintptr_t svc = 0;
    int lo = 0, hi = -1;
    bool all = false;
    uintptr_t begin[kGroupCount] = {}, end[kGroupCount] = {};
    bool activeBit[kGroupCount] = {};
    bool ok = false;
};

bool ReadLayout(Layout& l) {
    l = Layout{};
    l.svc = SvcAddr();
    if (!l.svc) return false;
    uint32_t lo = 0, hi = 0, pp = 0, base = 0;
    uint8_t flags = 0;
    if (!Rd(l.svc + 0xdc, lo) || !Rd(l.svc + 0xe0, hi) || !Rd(l.svc + 0xe4, pp) || !pp || !Rd(pp, base) || !base ||
        !Rd(l.svc + 0x75, flags))
        return false;
    l.all = (flags & 4) != 0;
    l.lo = static_cast<int>(lo < kGroupCount ? lo : kGroupCount);
    l.hi = hi < kGroupCount ? static_cast<int>(hi) : kGroupCount - 1;  // the flush loop is inclusive: lo..hi
    for (int i = 0; i < kGroupCount; ++i) {
        const uintptr_t g = base + i * kGroupStride;
        uint32_t b = 0, e = 0;
        uint8_t act = 0;
        if (!Rd(g + 0x20, b) || !Rd(g + 0x24, e) || !Rd(g + 0x2c, act)) continue;
        l.begin[i] = b;
        l.end[i] = e;
        l.activeBit[i] = (act & 1) != 0;
    }
    l.ok = true;
    return true;
}

bool GroupActive(const Layout& l, int i) { return i >= l.lo && i <= l.hi && (l.all || l.activeBit[i]); }

bool ReadStr(uintptr_t a, char (&out)[48]) {
    if (melange::mem::SafeRead(a, out, sizeof out)) {
        out[sizeof out - 1] = 0;
        return true;
    }
    for (size_t i = 0; i < sizeof out - 1; ++i) {  // near the end of a mapped page
        if (!Rd(a + i, out[i])) return false;
        if (!out[i]) return true;
    }
    out[sizeof out - 1] = 0;
    return true;
}

struct Mapping {
    uintptr_t addr = 0;
    uint32_t type = 0;
    char name[48] = {};
};

// The message id object is embedded at mapping+0 (the flush calls 0x68bbe6 with ECX = the mapping): u16 id, +2 flags,
// +4 name pointer.
bool ReadMapping(uintptr_t m, Mapping& out) {
    uint32_t nameptr = 0;
    out.addr = m;
    out.name[0] = 0;
    if (!Rd(m + 8, out.type) || !Rd(m + 4, nameptr) || !nameptr) return false;
    return ReadStr(nameptr, out.name);
}

Kind KindOf(const char* name) {
    if (strcmp(name, "Camera.MouseMoved") == 0) return kCamera;
    if (strcmp(name, "SpectateCam.MouseMoved") == 0) return kSpectate;
    if (strcmp(name, "Input.AimMouse") == 0) return kAim;
    return kNone;
}

// f(groupIndex, Mapping&) for every readable mapping of every group, in index order. Stops when f returns true.
template <class F>
void ForEachMapping(const Layout& l, F&& f) {
    for (int i = 0; i < kGroupCount; ++i) {
        if (!l.begin[i] || l.end[i] < l.begin[i]) continue;
        int n = 0;
        for (uintptr_t p = l.begin[i]; p < l.end[i] && n < 256; p += 4, ++n) {
            uint32_t m = 0;
            Mapping mp;
            if (!Rd(p, m) || !m || !ReadMapping(m, mp)) continue;
            if (f(i, mp)) return;
        }
    }
}

// Test verb controls.dump: every mapping of every group with its type and the dwords at +0xc/+0x10, to read key and
// button layouts from the live game.
bool VerbDump(std::string_view, void*) {
    Layout l;
    if (!ReadLayout(l)) {
        LOG_INFO("[controls] dump: no input service");
        return false;
    }
    int n = 0;
    ForEachMapping(l, [&](int gi, Mapping& m) {
        uint32_t c = 0, d = 0;
        Rd(m.addr + 0xc, c);
        Rd(m.addr + 0x10, d);
        LOG_INFO("[controls] dump g%02d %-22s%s type=%u %-32s +c=%08x +10=%08x", gi, kGroupNames[gi],
                 GroupActive(l, gi) ? "*" : " ", m.type, m.name, c, d);
        ++n;
        return false;
    });
    LOG_INFO("[controls] dump: %d mappings, groups %d..%d all=%d", n, l.lo, l.hi, l.all);
    return true;
}

// ---------------------------------------------------------------- invert overrides
struct Tracked {
    uint8_t orig, written;
};
std::unordered_map<uintptr_t, Tracked> g_tracked;  // main thread
std::atomic<int> g_camFlag{-1};                    // effective Camera.MouseMoved invertY flag, -1 unknown
std::atomic<int> g_aimFlag{-1};

void ApplyInvert() {
    const int camWant = InvertFlagFor(g_opt.cameraInvertY, kStandardCameraInvertY);
    const int aimWant = InvertFlagFor(g_opt.aimInvertY, kStandardAimInvertY);
    if (camWant < 0 && aimWant < 0 && !g_opt.blimpInvert && g_tracked.empty() && !g_probe) return;
    Layout l;
    if (!ReadLayout(l)) return;
    std::unordered_map<uintptr_t, Tracked> next;
    int camActive = -1, camAny = -1, aimAny = -1;
    ForEachMapping(l, [&](int gi, Mapping& m) {
        const Kind k = KindOf(m.name);
        if (m.type != 3 || (k != kCamera && k != kAim)) return false;
        uint8_t cur = 0;
        if (!Rd(m.addr + 0xd, cur)) return false;
        const auto it = g_tracked.find(m.addr);
        // The game bakes the flag when it builds the groups; a value that is not what we wrote means a rebuild.
        const uint8_t orig = (it != g_tracked.end() && it->second.written == cur) ? it->second.orig : cur;
        const int want = k == kCamera ? camWant : aimWant;
        const uint8_t now = want >= 0 ? static_cast<uint8_t>(want) : orig;
        if (now != cur) melange::mem::Put<uint8_t>(m.addr + 0xd, now);
        // Only overridden mappings are tracked: once every mode is back to "game" the table empties and the scan stops.
        if (want >= 0) next[m.addr] = {orig, now};
        if (k == kCamera) {
            if (camAny < 0) camAny = now;
            if (camActive < 0 && GroupActive(l, gi)) camActive = now;
        } else if (aimAny < 0) {
            aimAny = now;
        }
        return false;
    });
    g_tracked.swap(next);
    g_camFlag = camActive >= 0 ? camActive : camAny;
    g_aimFlag = aimAny;
}

void RestoreInvert() {
    for (const auto& [addr, t] : g_tracked) {
        uint8_t cur = 0;
        if (t.written != t.orig && Rd(addr + 0xd, cur) && cur == t.written) melange::mem::Put<uint8_t>(addr + 0xd, t.orig);
    }
    g_tracked.clear();
    g_camFlag = g_aimFlag = -1;
}

// ---------------------------------------------------------------- message ids, blimp, sensitivity
std::atomic<int> g_idCamera{-1}, g_idSpectate{-1}, g_idAim{-1};
uint64_t g_idTry = 0;
Carry g_carryX[4], g_carryY[4];
// The flush posts a message once per active group that maps it, all with the same motion. Every copy within one flush
// gets the same scaled result, so the carry advances once per flush and not once per mapping.
struct PostMemo {
    uint32_t flush = 0;
    int inX = 0, inY = 0, outX = 0, outY = 0;
    bool valid = false;
};
uint32_t g_flushSeq = 0;  // main thread, like the flush
PostMemo g_memo[4];

void ResolveIds() {
    if (g_idCamera >= 0 && g_idSpectate >= 0 && g_idAim >= 0) return;
    const uint64_t now = GetTickCount64();
    if (now - g_idTry < 100 || !melange::bus::RegistryReady()) return;
    g_idTry = now;
    auto get = [](const char* n, std::atomic<int>& slot) {
        if (slot >= 0) return;
        const auto id = melange::bus::IdOf(n);
        if (id != melange::bus::kInvalidId) slot = id;
    };
    get("Camera.MouseMoved", g_idCamera);
    get("SpectateCam.MouseMoved", g_idSpectate);
    get("Input.AimMouse", g_idAim);
}

Kind KindOfId(uint16_t id) {
    if (static_cast<int>(id) == g_idCamera.load(std::memory_order_relaxed)) return kCamera;
    if (static_cast<int>(id) == g_idAim.load(std::memory_order_relaxed)) return kAim;
    if (static_cast<int>(id) == g_idSpectate.load(std::memory_order_relaxed)) return kSpectate;
    return kNone;
}

bool InBlimpView() {
    uint32_t cm = 0;
    if (!Rd(kCameraMgrPtr, cm) || !cm) return false;
    __try {
        return reinterpret_cast<uint8_t(__fastcall*)(void*, void*)>(kBlimpView)(reinterpret_cast<void*>(cm), nullptr) == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Mid hook at the entry of 0x505990: ctx.esp -> return address, then msgId, x, y (the callee pops them).
void OnPost(SafetyHookContext& ctx) {
    ResolveIds();
    uint32_t* a = reinterpret_cast<uint32_t*>(ctx.esp);
    const Kind k = KindOfId(static_cast<uint16_t>(a[1]));
    if (k == kNone) return;
    const int inX = static_cast<int>(a[2]), inY = static_cast<int>(a[3]);
    PostMemo& memo = g_memo[k];
    if (memo.valid && memo.flush == g_flushSeq && memo.inX == inX && memo.inY == inY) {
        a[2] = static_cast<uint32_t>(memo.outX);
        a[3] = static_cast<uint32_t>(memo.outY);
        return;
    }
    int x = inX, y = inY;
    if (k == kCamera && g_opt.blimpInvert && g_camFlag.load(std::memory_order_relaxed) == 1 && InBlimpView())
        y = -y;  // the flush skips the invert flags in this view; do what it does everywhere else
    const float s = k == kAim ? g_opt.aimSensitivity : g_opt.cameraSensitivity;
    if (s != 1.0f || g_carryX[k].residue != 0.0f || g_carryY[k].residue != 0.0f) {
        x = g_carryX[k].Apply(x, s);
        y = g_carryY[k].Apply(y, s);
    }
    memo = {g_flushSeq, inX, inY, x, y, true};
    a[2] = static_cast<uint32_t>(x);
    a[3] = static_cast<uint32_t>(y);
}

// ---------------------------------------------------------------- Raw Input smoothing
std::atomic<int> g_rawX{0}, g_rawY{0};
std::atomic<uint32_t> g_rawEvents{0};  // accepted events since the last flush
std::atomic<long long> g_rawTotX{0}, g_rawTotY{0};
std::atomic<bool> g_rawSeen{false}, g_rawRegistered{false};
HWND g_rawHwnd = nullptr;
Carry g_smoothX, g_smoothY;
RawFallback g_rawFallback;            // touched by the flush only
std::atomic<bool> g_rawStale{false};  // mirror of g_rawFallback.stale for OnFrame

bool SmoothActive() {
    return g_smoothCfg && g_rawRegistered.load(std::memory_order_relaxed) && g_rawSeen.load(std::memory_order_relaxed);
}

void AddRaw(LONG dx, LONG dy) {
    g_rawX.fetch_add(dx, std::memory_order_relaxed);
    g_rawY.fetch_add(dy, std::memory_order_relaxed);
    g_rawEvents.fetch_add(1, std::memory_order_relaxed);
    g_rawTotX.fetch_add(dx, std::memory_order_relaxed);
    g_rawTotY.fetch_add(dy, std::memory_order_relaxed);
}

// The game has the mouse: a window of this process (the game window, or one it owns) is the foreground one and the
// overlay is not capturing.
bool GameFocused(HWND h) {
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    if (fg != h && (!GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId())) return false;
    return !melange::overlay::Capturing();
}

void RawSink(HWND h, WPARAM wp, LPARAM lp) {
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;
    if (ri.header.dwType != RIM_TYPEMOUSE || (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) return;
    const LONG dx = ri.data.mouse.lLastX, dy = ri.data.mouse.lLastY;
    if (!dx && !dy) return;
    // Background input is ignored, and so is anything the overlay owns (the game gets no mouse then either).
    if (GET_RAWINPUT_CODE_WPARAM(wp) != RIM_INPUT || !GameFocused(h)) return;
    g_rawSeen.store(true, std::memory_order_relaxed);  // only an event that is used arms the smoothing
    AddRaw(dx, dy);
}

// Test verb controls.raw <dx> <dy>: adds relative counts exactly as RawSink does for a WM_INPUT event, without the
// foreground check, so the smoothing path can be driven by the automation queue (injected SendInput motion is dropped
// by Windows while an elevated window has the focus).
bool VerbRaw(std::string_view args, void*) {
    int dx = 0, dy = 0;
    if (sscanf_s(std::string(args).c_str(), "%d %d", &dx, &dy) != 2 || !g_rawRegistered) return false;
    g_rawSeen.store(true, std::memory_order_relaxed);
    AddRaw(dx, dy);
    return true;
}

// Mid hook at the entry of 0x5060e0: raw motion alone must still make the service flush on the next tick.
void OnTick(SafetyHookContext& ctx) {
    if (!SmoothActive() || (!g_rawX.load(std::memory_order_relaxed) && !g_rawY.load(std::memory_order_relaxed))) return;
    uint32_t svc = 0;
    if (!Rd(ctx.esp + 4, svc) || !svc || svc != SvcAddr()) return;
    *reinterpret_cast<uint8_t*>(svc + 0x75) |= 1;
}

// Mid hook at the entry of 0x505c70 (ecx = service): the game's motion for this flush comes from Raw Input instead
// of the cursor deltas, which the per-frame cursor warp makes jitter.
void OnFlush(SafetyHookContext& ctx) {
    uint8_t* svc = reinterpret_cast<uint8_t*>(ctx.ecx);
    if (!(svc[0x75] & 1)) return;  // the flush returns at once
    ++g_flushSeq;
    ApplyInvert();
    if (!SmoothActive()) return;
    const float sens = *reinterpret_cast<float*>(svc + 0xa4);
    const int rawX = g_rawX.exchange(0, std::memory_order_relaxed), rawY = g_rawY.exchange(0, std::memory_order_relaxed);
    int* gameX = reinterpret_cast<int*>(svc + 0x78);
    int* gameY = reinterpret_cast<int*>(svc + 0x7c);
    const uint32_t events = g_rawEvents.exchange(0, std::memory_order_relaxed);
    const RawFallback::Result r =
        g_rawFallback.Feed(events, *gameX, *gameY, g_rawHwnd && GameFocused(g_rawHwnd), GetTickCount64());
    g_rawStale.store(g_rawFallback.stale, std::memory_order_relaxed);
    if (r.becameStale || r.rearmed) {
        g_smoothX.Reset();  // no sub-count residue carried across the switch
        g_smoothY.Reset();
    }
    if (r.becameStale)
        LOG_WARN("[controls] Raw Input stopped arriving while the mouse moves; using the game's own mouse input");
    if (r.rearmed) LOG_INFO("[controls] Raw Input is arriving again; smoothing resumed");
    if (!r.useRaw) return;  // stale: the game's own deltas stand
    *gameX = g_smoothX.Apply(rawX, sens);
    *gameY = g_smoothY.Apply(rawY, sens);
}

void RegisterRaw(HWND hwnd, bool quiet = false) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 1;
    rid.usUsage = 2;  // mouse; no RIDEV_INPUTSINK: only while the game window is the foreground one
    rid.hwndTarget = hwnd;
    g_rawHwnd = hwnd;  // one attempt per window
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        if (!quiet)
            LOG_WARN("[controls] RegisterRawInputDevices failed (error %lu): SmoothMouse off, the game's own mouse input is used",
                     GetLastError());
        return;
    }
    g_rawRegistered = true;
    melange::render::SetRawInputSink(&RawSink);
    if (!quiet) LOG_INFO("[controls] Raw Input mouse registered on window %p", static_cast<void*>(hwnd));
}

void UnregisterRaw() {
    melange::render::SetRawInputSink(nullptr);
    if (g_rawRegistered.exchange(false)) {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 1;
        rid.usUsage = 2;
        rid.dwFlags = RIDEV_REMOVE;
        RegisterRawInputDevices(&rid, 1, sizeof rid);
    }
    g_rawHwnd = nullptr;
    g_rawX = 0;
    g_rawY = 0;
    g_rawEvents = 0;
    g_rawSeen = false;
    g_rawStale = false;
    g_rawFallback.Reset();
}

void ProbeLog() {
    static int lastCam = -2, lastAim = -2, lastTracked = -1, lastSeen = -1, lastStale = -1;
    static long long lastX = 0, lastY = 0;
    static uint64_t lastRaw = 0;
    const int cam = g_camFlag, aim = g_aimFlag, seen = g_rawSeen ? 1 : 0;
    const int tracked = static_cast<int>(g_tracked.size());
    const int stale = g_rawStale.load(std::memory_order_relaxed) ? 1 : 0;
    if (cam != lastCam || aim != lastAim || tracked != lastTracked || seen != lastSeen || stale != lastStale) {
        lastStale = stale;
        lastCam = cam;
        lastAim = aim;
        lastTracked = tracked;
        lastSeen = seen;
        LOG_INFO("[controls] probe: Camera.MouseMoved invertY=%d, Input.AimMouse invertY=%d, %d mappings tracked, rawSeen=%d smooth=%d stale=%d",
                 cam, aim, tracked, seen, SmoothActive(), stale);
    }
    const uint64_t now = GetTickCount64();
    const long long tx = g_rawTotX, ty = g_rawTotY;
    if ((tx != lastX || ty != lastY) && now - lastRaw >= 1000) {
        lastX = tx;
        lastY = ty;
        lastRaw = now;
        LOG_INFO("[controls] probe: raw mouse totals x=%lld y=%lld", tx, ty);
    }
}

void OnFrame() {
    if (!g_installed) return;
    HWND hwnd = static_cast<HWND>(melange::events::GameWindow());
    if (g_smoothCfg && hwnd && hwnd != g_rawHwnd && melange::render::SubclassedWindow() == hwnd) RegisterRaw(hwnd);
    // While Raw Input is stale something may have replaced our registration: register again, quietly, every 5 s.
    if (g_smoothCfg && hwnd && hwnd == g_rawHwnd && g_rawStale.load(std::memory_order_relaxed)) {
        static uint64_t lastTry = 0;
        const uint64_t now = GetTickCount64();
        if (now - lastTry >= 5000) {
            lastTry = now;
            RegisterRaw(hwnd, true);
        }
    }
    // The flush re-applies the flags right before it reads them; the per-frame scan is only for the probe log.
    if (g_probe) {
        ApplyInvert();
        ProbeLog();
    }
}

bool Hook(uintptr_t at, safetyhook::MidHookFn fn, const char* what) {
    auto h = safetyhook::create_mid(at, fn);
    if (!h) {
        LOG_ERROR("[controls] hook %s at %08x failed", what, static_cast<unsigned>(at));
        return false;
    }
    g_hooks.push_back(std::move(h));
    return true;
}

class Controls final : public melange::Module {
public:
    const char* Name() const override { return "Controls"; }
    const char* Description() const override { return "wum.input: invert and sensitivity options, Raw Input mouse smoothing"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 45; }

    bool Install() override {
        g_smoothCfg = Bool("SmoothMouse", true);
        g_probe = Bool("Probe", false);
        namespace mem = melange::mem;
        // 0x5085ab: mov [InputTranslationService],ebx in the service constructor (the instance pointer global). Not
        // 0x705fb4, which reads it too: the Fixes module hooks that site.
        if (!mem::Expect(0x5085ab, {0x89, 0x1d, 0xa4, 0xb2, 0x95, 0x00}) ||
            // 0x505c70 flush: sub esp,0Ch; push esi; mov esi,ecx; test byte [esi+75h],1
            !mem::Expect(kFlush, {0x83, 0xec, 0x0c, 0x56, 0x8b, 0xf1, 0xf6, 0x46, 0x75, 0x01}) ||
            // group range svc+0E0h / +0DCh
            !mem::Expect(0x505cbb, {0x8b, 0x86, 0xe0, 0x00, 0x00, 0x00, 0x8b, 0x8e, 0xdc, 0x00, 0x00, 0x00}) ||
            // group vector base **(svc+0E4h), group+2Ch active bit, mapping vector group+20h..+24h
            !mem::Expect(0x505cf0, {0x8b, 0x86, 0xe4, 0x00, 0x00, 0x00, 0x8b, 0x28, 0x03, 0xef, 0x84, 0xd2, 0x75, 0x06, 0xf6,
                                    0x45, 0x2c, 0x01, 0x74, 0x76, 0x8b, 0x5d, 0x20, 0x3b, 0x5d, 0x24}) ||
            // blimp test: mov ecx,[CameraManager]; test ecx,ecx; jz; call
            !mem::Expect(0x505d18, {0x8b, 0x0d, 0x70, 0xc3, 0x95, 0x00, 0x85, 0xc9, 0x74, 0x1c, 0xe8}) ||
            // invert flags: cmp byte [edi+0Dh],0 ... neg ecx; cmp byte [edi+0Ch],0
            !mem::Expect(0x505d3e, {0x80, 0x7f, 0x0d, 0x00, 0x8b, 0x4e, 0x7c, 0x74, 0x02, 0xf7, 0xd9, 0x80, 0x7f, 0x0c, 0x00}) ||
            // 0x505990 poster: push -1; push SEH handler; mov eax,fs:[0]
            !mem::Expect(kPost, {0x6a, 0xff, 0x68, 0x9b, 0x32, 0x7d, 0x00, 0x64, 0xa1}) ||
            // 0x5060e0 tick: mov eax,fs:[0]; push -1; push SEH handler; push eax; mov fs:[0],esp; push esi; mov esi,[esp+14h]
            !mem::Expect(kTick, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0xd8, 0xbe, 0x7d, 0x00, 0x50, 0x64, 0x89,
                                 0x25, 0x00, 0x00, 0x00, 0x00, 0x56, 0x8b, 0x74, 0x24, 0x14}) ||
            // 0x51d8f0 blimp-like view predicate
            !mem::Expect(kBlimpView, {0x56, 0x8b, 0xf1, 0x83, 0xbe, 0xbc, 0x02, 0x00, 0x00, 0xff, 0x74, 0x04, 0x32, 0xc0, 0x5e, 0xc3})) {
            LOG_ERROR("[controls] unexpected code in the input translation service, not installed");
            return false;
        }
        if (!Hook(kPost, &OnPost, "post") || !Hook(kFlush, &OnFlush, "flush") || !Hook(kTick, &OnTick, "tick")) {
            g_hooks.clear();
            return false;
        }
        g_installed = true;
        melange::testcmd::Register("controls.dump", &VerbDump);
        melange::testcmd::Register("controls.raw", &VerbRaw);
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        melange::events::Subscribe(melange::events::Event::Shutdown, [] {
            g_opt = Options{};  // the hooks outlive this event; stop them re-applying the overrides
            RestoreInvert();
            UnregisterRaw();
        });
        LOG_INFO("[controls] installed: mid hooks at %08x (mouse post: invert, blimp, sensitivity), %08x (flush: Raw Input "
                 "motion, invert re-apply), %08x (tick); SmoothMouse=%d Probe=%d",
                 static_cast<unsigned>(kPost), static_cast<unsigned>(kFlush), static_cast<unsigned>(kTick), g_smoothCfg, g_probe);
        return true;
    }

    void Uninstall() override {
        g_installed = false;
        g_hooks.clear();
        RestoreInvert();
        UnregisterRaw();
        g_opt = Options{};
        g_owner = nullptr;
    }
};
MELANGE_MODULE(Controls);
}  // namespace

bool Available() { return g_installed; }

void SetOptions(const Options& o, const void* owner) {
    g_opt = o;
    g_owner = owner;
    for (Carry& c : g_carryX) c.Reset();
    for (Carry& c : g_carryY) c.Reset();
    for (PostMemo& m : g_memo) m.valid = false;
    if (g_installed) ApplyInvert();
}

void ClearAllOptions() { SetOptions(Options{}, nullptr); }

void ClearOptions(const void* owner) {
    if (owner && g_owner == owner) ClearAllOptions();
}

Options Effective() { return g_opt; }

bool SmoothMouse() { return g_smoothCfg && g_rawRegistered.load(); }

bool ActiveGroups(std::vector<std::string>* out) {
    if (!g_installed) return false;
    out->clear();
    Layout l;
    if (!ReadLayout(l)) return true;  // no service yet
    for (int i = 0; i < kGroupCount; ++i)
        if (GroupActive(l, i)) out->push_back(kGroupNames[i]);
    return true;
}

bool Binding(const char* messageName, std::string* label) {
    if (!g_installed || !messageName) return false;
    Layout l;
    if (!ReadLayout(l)) return false;
    bool found = false;
    // Mapping types read from the live table (controls.dump): 0 keyboard, +0xc = DIK scan code; 5 mouse button,
    // +0xc = 0 left, 1 middle, 2 right; 1 and 2 are joypad axis/button mappings and are skipped. A keyboard binding
    // wins over a mouse one; the first of each kind in group order is used.
    std::string mouse;
    ForEachMapping(l, [&](int, Mapping& m) {
        if ((m.type != 0 && m.type != 5) || strcmp(m.name, messageName) != 0) return false;
        uint32_t code = 0;
        if (!Rd(m.addr + 0xc, code)) return false;
        if (m.type == 5) {
            static const char* const kButtons[] = {"LMB", "MMB", "RMB"};
            if (code < 3 && mouse.empty()) mouse = kButtons[code];
            return false;
        }
        if (code == 0 || code > 0xFF) return false;
        const char* n = melange::render::DikName(static_cast<uint8_t>(code));
        if (n[0] == '?') return false;
        *label = KeyLabel(n);
        found = true;
        return true;
    });
    if (!found && !mouse.empty()) {
        *label = mouse;
        found = true;
    }
    return found;
}
}  // namespace melange::controls
