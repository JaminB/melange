#include "weapons/engine.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <initializer_list>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"

namespace melange::weapons::engine {
namespace {
constexpr uintptr_t kRoot = 0x639b1d, kDrmIid = 0x888288, kLookup = 0x50b8b0, kDescLookup = 0x50b760;
constexpr uintptr_t kXStrCtor = 0x638101, kXStrFree = 0x637db4, kXStrAssign = 0x638828, kApp = 0x96d1cc;
constexpr uintptr_t kAddResource = 0x6a5a70, kLoadBank = 0x6a37a0, kAddString = 0x6a5830, kTextGet = 0x50b820;
constexpr int kSlotAddResource = 9, kSlotLoadBank = 42, kSlotAddString = 10;

struct Site {
    uintptr_t addr;
    std::initializer_list<int> bytes;
    bool hook;
};
const Site kSites[] = {
    {kRoot, {0x83, 0x3d, 0x98, 0x6d, 0x96, 0x00, 0x00}, false},
    {kLookup, {0xe8, 0x68, 0xe2, 0x12, 0x00, 0x8b, 0x08}, false},
    {kDescLookup, {0xe8, 0xb8, 0xe3, 0x12, 0x00, 0x8b, 0x08}, false},
    {kXStrCtor, {0x55, 0x8b, 0x6c, 0x24, 0x08, 0x57, 0x8b, 0xf9}, false},
    {kXStrFree, {0x51, 0xff, 0x15, 0x6c, 0x51, 0x81, 0x00, 0x59, 0xc3}, false},
    {kXStrAssign, {0xe9, 0x82, 0xfe, 0xff, 0xff}, false},
    {kAddResource, {0x83, 0xec, 0x10, 0x53, 0x8b, 0x5c, 0x24, 0x1c}, false},
    {kLoadBank, {0x83, 0xec, 0x18, 0x53, 0x55, 0x56, 0x8b, 0x74, 0x24, 0x28}, false},
    {kAddString, {0x55, 0x8b, 0x6c, 0x24, 0x0c, 0x8b, 0x45, 0x00}, false},
    {kTextGet, {0x53, 0x56, 0x57, 0xe8, 0xf5, 0xe2, 0x12, 0x00}, false},
    {kSelWrite, {0x89, 0xa8, 0xf4, 0x00, 0x00, 0x00}, true},
    {kSelLog, {0x8b, 0x04, 0x85, 0x20, 0xc9, 0x90, 0x00}, true},
    {kInvGet, {0x83, 0xf8, 0x41, 0x0f, 0x87}, true},
    {kAllowed, {0x83, 0xf8, 0x41, 0x0f, 0x87}, true},
    {kDelay, {0x83, 0xf8, 0x41, 0x0f, 0x87}, true},
    {kCanUse, {0x33, 0xf6, 0x52, 0x89, 0x74, 0x24, 0x14}, true},
    {kText, {0x51, 0x8d, 0x54, 0x24, 0x10}, true},
    {kHelp, {0x53, 0x52, 0x68, 0xc4, 0x7e, 0x86, 0x00}, true},
    {kHudIcon, {0x8b, 0x44, 0x24, 0x04, 0x50}, true},
    {kLaunch, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0xd1, 0x57, 0x7d}, true},
    {kUpdBase, {0xe8, 0x5b, 0x8a, 0xf6, 0xff, 0x84, 0xc0}, true},
    {kUpdPara, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0xa6, 0x50, 0x7d}, true},
    {kMsgBase, {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x10, 0x0f, 0xb7, 0x77, 0x04}, true},
    {kMsgPara, {0x83, 0xec, 0x2c, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x3c}, true},
    {kExplode, {0x6a, 0xff, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x68, 0xcb, 0x54, 0x7d}, true},
    {kUpload, {0x83, 0xec, 0x28, 0x55, 0x56, 0x8b, 0x74, 0x24, 0x34}, true},
    {kStartCheck, {0x3b, 0xf8, 0x0f, 0x85, 0xbc, 0x00, 0x00, 0x00}, true},
};

bool Intact(const Site& s) { return game::IsKnownBuild() && mem::Expect(s.addr, s.bytes); }

struct ExtraSite {
    uintptr_t addr;
    std::vector<int> bytes;
};
std::vector<ExtraSite> g_extraSites;

bool Intact(const ExtraSite& s) {
    if (!game::IsKnownBuild()) return false;
    for (size_t i = 0; i < s.bytes.size(); ++i) {
        uint8_t b = 0;
        if (s.bytes[i] >= 0 && (!mem::SafeRead(s.addr + i, &b, 1) || b != s.bytes[i])) return false;
    }
    return true;
}

bool FnOk(uintptr_t addr) {
    for (auto& s : kSites)
        if (s.addr == addr) return Intact(s);
    return false;
}

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

uintptr_t Slot(uintptr_t obj, int slot) { return Rd<uintptr_t>(Rd<uintptr_t>(obj) + slot * 4); }

uintptr_t RawDrm() {
    __try {
        const uintptr_t root = reinterpret_cast<uintptr_t(__cdecl*)()>(kRoot)();
        if (!root) return 0;
        auto qi = reinterpret_cast<uintptr_t(__stdcall*)(uintptr_t, uintptr_t)>(Slot(root, 0x54 / 4));
        return qi ? qi(root, kDrmIid) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uintptr_t RawLookup(uintptr_t fn, const char* name) {
    uintptr_t out = 0;
    __try {
        reinterpret_cast<void(__cdecl*)(const char**, uintptr_t*)>(fn)(&name, &out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return out;
}

int RawAdd(uintptr_t drm, uintptr_t fn, const char* name, uintptr_t obj, uint16_t section, uint32_t flags) {
    __try {
        return reinterpret_cast<int(__stdcall*)(uintptr_t, const char**, uintptr_t, uint32_t, uint32_t)>(fn)(
            drm, &name, obj, section, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int RawLoadBank(uintptr_t drm, uintptr_t fn, const char* path, uint32_t section, uint32_t flags) {
    __try {
        return reinterpret_cast<int(__stdcall*)(uintptr_t, const char*, uint32_t, uint32_t)>(fn)(drm, path, section, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int RawAddString(uintptr_t drm, uintptr_t fn, const char* name, const char* value, uint32_t section, uint32_t flags) {
    __try {
        return reinterpret_cast<int(__stdcall*)(uintptr_t, const char**, const char*, uint32_t, uint32_t)>(fn)(
            drm, &name, value, section, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int RawTextGet(const char* name, const char** out) {
    __try {
        return reinterpret_cast<int(__cdecl*)(const char**, const char**)>(kTextGet)(&name, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

uintptr_t RawClassOf(uintptr_t c) {
    __try {
        auto fn = reinterpret_cast<uintptr_t(__stdcall*)(uintptr_t)>(Slot(c, 3));
        return fn ? fn(c) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool RawCtor(uintptr_t* p, const char* s) {
    __try {
        reinterpret_cast<void(__thiscall*)(uintptr_t*, const char*)>(kXStrCtor)(p, s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *p = 0;
        return false;
    }
    return true;
}

void RawRelease(uintptr_t p) {
    __try {
        auto* refs = reinterpret_cast<uint16_t*>(p - 6);
        if (*refs == 0xffff || *refs == 0) return;
        if (--*refs == 0) reinterpret_cast<void(__thiscall*)(uintptr_t)>(kXStrFree)(p - 6);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool RawAssign(uintptr_t field, const char* s) {
    __try {
        reinterpret_cast<void(__thiscall*)(uintptr_t, const char*)>(kXStrAssign)(field, s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool RawAddPath(uintptr_t app, uintptr_t* xs) {
    __try {
        reinterpret_cast<void(__stdcall*)(uintptr_t, uintptr_t*)>(Slot(app, 3))(app, xs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

struct Tracked {
    std::string name;
    uintptr_t site;
    SafetyHookMid* mid;
    SafetyHookInline* in;
    bool wanted;
};
std::vector<Tracked> g_hooks;
bool g_suppressed = false;

Tracked* Find(const void* h) {
    for (auto& t : g_hooks)
        if (t.mid == h || t.in == h) return &t;
    return nullptr;
}

void Apply(Tracked& t) {
    const bool on = t.wanted && !g_suppressed;
    bool ok = true;
    if (t.mid && *t.mid && t.mid->enabled() != on) ok = on ? t.mid->enable().has_value() : t.mid->disable().has_value();
    if (t.in && *t.in && t.in->enabled() != on) ok = on ? t.in->enable().has_value() : t.in->disable().has_value();
    if (!ok) LOG_ERROR("[weapons] %s the %s hook failed", on ? "enabling" : "disabling", t.name.c_str());
}

void Forget(const void* h) {
    std::erase_if(g_hooks, [h](const Tracked& t) { return t.mid == h || t.in == h; });
}
}  // namespace

bool SitesOk() {
    bool ok = true;
    for (auto& s : kSites)
        if (!Intact(s)) {
            LOG_ERROR("[weapons] code bytes at %08x differ from build #1077", static_cast<unsigned>(s.addr));
            ok = false;
        }
    return ok;
}

bool SiteIntact(uintptr_t site) {
    for (auto& s : kSites)
        if (s.addr == site) return s.hook && Intact(s);
    for (auto& s : g_extraSites)
        if (s.addr == site) return Intact(s);
    return false;
}

void AddHookSite(uintptr_t site, std::initializer_list<int> bytes) {
    for (auto& s : kSites)
        if (s.addr == site) return;
    for (auto& s : g_extraSites)
        if (s.addr == site) return;
    g_extraSites.push_back({site, std::vector<int>(bytes)});
}

uintptr_t Drm() { return FnOk(kRoot) ? RawDrm() : 0; }

uintptr_t Lookup(const char* name) { return name && *name && FnOk(kLookup) ? RawLookup(kLookup, name) : 0; }

uintptr_t LookupDesc(const char* name) { return name && *name && FnOk(kDescLookup) ? RawLookup(kDescLookup, name) : 0; }

int AddResource(const char* name, uintptr_t obj, uint16_t section, uint32_t flags) {
    if (!name || !*name || !obj || !FnOk(kAddResource)) return -1;
    const uintptr_t drm = Drm();
    if (!drm) return -1;
    const uintptr_t fn = Slot(drm, kSlotAddResource);
    if (fn != kAddResource) {
        LOG_ERROR("[weapons] AddResource slot is %08x, expected %08x", static_cast<unsigned>(fn), static_cast<unsigned>(kAddResource));
        return -1;
    }
    return RawAdd(drm, fn, name, obj, section, flags);
}

int LoadBank(const char* gameRelPath, uint32_t section, uint32_t flags) {
    if (!gameRelPath || !*gameRelPath || !FnOk(kLoadBank)) return -1;
    const uintptr_t drm = Drm();
    if (!drm) return -1;
    const uintptr_t fn = Slot(drm, kSlotLoadBank);
    if (fn != kLoadBank) {
        LOG_ERROR("[weapons] LoadBank slot is %08x, expected %08x", static_cast<unsigned>(fn), static_cast<unsigned>(kLoadBank));
        return -1;
    }
    return RawLoadBank(drm, fn, gameRelPath, section, flags);
}

uintptr_t ClassOf(uintptr_t container) { return container ? RawClassOf(container) : 0; }

int AddString(const char* name, const char* value, uint32_t section, uint32_t flags) {
    if (!name || !*name || !value || !FnOk(kAddString)) return -1;
    const uintptr_t drm = Drm();
    if (!drm) return -1;
    const uintptr_t fn = Slot(drm, kSlotAddString);
    if (fn != kAddString) {
        LOG_ERROR("[weapons] AddString slot is %08x, expected %08x", static_cast<unsigned>(fn), static_cast<unsigned>(kAddString));
        return -1;
    }
    return RawAddString(drm, fn, name, value, section, flags);
}

bool TextOf(const char* name, std::string* out) {
    if (!name || !*name || !FnOk(kTextGet)) return false;
    const char* v = nullptr;
    if (RawTextGet(name, &v) < 0 || !v) return false;
    if (out) *out = ReadCString(reinterpret_cast<uintptr_t>(v), 512);
    return true;
}

XStr::XStr(const char* s) {
    if (FnOk(kXStrCtor)) RawCtor(&p, s);
}

XStr::~XStr() {
    if (p && FnOk(kXStrFree)) RawRelease(p);
}

bool AssignXString(uintptr_t field, const char* s) {
    if (!field || !s || !FnOk(kXStrAssign) || !Rd<uintptr_t>(field)) return false;
    return RawAssign(field, s);
}

const char* EnumName(int id) {
    if (id < 0 || id >= kEnumCount) return nullptr;
    return reinterpret_cast<const char*>(Rd<uintptr_t>(kNames + 4 * id));
}

bool AddSearchPath(const char* gameRelDir) {
    if (!gameRelDir || !*gameRelDir || !game::IsKnownBuild()) return false;
    const uintptr_t app = Rd<uintptr_t>(kApp);
    if (!app) return false;
    XStr s(gameRelDir);
    return s.p && RawAddPath(app, &s.p);
}

bool AppReady() { return game::IsKnownBuild() && Rd<uintptr_t>(kApp) != 0; }

std::string ReadCString(uintptr_t p, size_t max) {
    std::string s;
    char c = 0;
    while (p && s.size() < max && mem::SafeRead(p + s.size(), &c, 1) && c) s.push_back(c);
    return s;
}

std::string XStringValue(uintptr_t field) { return ReadCString(Rd<uintptr_t>(field), 256); }

std::string ClassName(uintptr_t cls) {
    const uintptr_t info = cls ? Rd<uintptr_t>(cls + 0x10) : 0;
    return info ? ReadCString(Rd<uintptr_t>(info), 96) : std::string();
}

uintptr_t ClassParent(uintptr_t cls) { return cls ? Rd<uintptr_t>(cls + 0x14) : 0; }

bool Mid(SafetyHookMid& h, uintptr_t site, safetyhook::MidHookFn fn, const char* name) {
    if (h) return true;
    if (!SiteIntact(site)) {
        LOG_ERROR("[weapons] %s: the bytes at %08x are not #1077's (changed by another module?): refused", name,
                  static_cast<unsigned>(site));
        return false;
    }
    auto r = safetyhook::MidHook::create(reinterpret_cast<void*>(site), fn, safetyhook::MidHook::StartDisabled);
    if (!r) {
        LOG_ERROR("[weapons] creating the %s hook at %08x failed", name, static_cast<unsigned>(site));
        return false;
    }
    h = std::move(*r);
    Forget(&h);
    g_hooks.push_back({name, site, &h, nullptr, false});
    return true;
}

bool InlineRaw(SafetyHookInline& h, uintptr_t site, void* fn, const char* name) {
    if (h) return true;
    if (!SiteIntact(site)) {
        LOG_ERROR("[weapons] %s: the bytes at %08x are not #1077's (changed by another module?): refused", name,
                  static_cast<unsigned>(site));
        return false;
    }
    auto r = safetyhook::InlineHook::create(reinterpret_cast<void*>(site), fn, safetyhook::InlineHook::StartDisabled);
    if (!r) {
        LOG_ERROR("[weapons] creating the %s hook at %08x failed", name, static_cast<unsigned>(site));
        return false;
    }
    h = std::move(*r);
    Forget(&h);
    g_hooks.push_back({name, site, nullptr, &h, false});
    return true;
}

void Enable(SafetyHookMid& h, bool on) {
    if (Tracked* t = Find(&h)) {
        t->wanted = on;
        Apply(*t);
    }
}

void Enable(SafetyHookInline& h, bool on) {
    if (Tracked* t = Find(&h)) {
        t->wanted = on;
        Apply(*t);
    }
}

void SuppressAll(bool suppress) {
    g_suppressed = suppress;
    for (auto& t : g_hooks) Apply(t);
    LOG_INFO("[weapons] hooks %s (%zu tracked)", suppress ? "suppressed" : "restored", g_hooks.size());
}

bool Suppressed() { return g_suppressed; }

// Vehicle meshes. BomberGraphicEntity::Setup (0x54cab0) and SuperBomberGraphicEntity::Setup (0x5897e0) each create their
// mesh with `push <&name>; call 0x6f3d95` (the GRM CreateResource wrapper: the name is passed as a pointer to a char*), where
// <&name> is a static `const char*` in .data that starts out pointing at "BomberHelicopter" / "SuperAirstrike" and that
// nothing else writes or reads. Pointing that variable at a mod mesh's name is therefore the whole hook: no code is patched.
namespace {
struct Vehicle {
    const char* key;          // the vehicleMeshes key, which is also the vanilla mesh name
    uintptr_t var;            // the static char*
    uint32_t vanilla;         // what the variable holds in #1077
    uintptr_t push;           // the `push imm32` that passes &var ...
    std::initializer_list<int> bytes;  // ... and the CreateResource wrapper it calls (a call to 0x6f3d95)
};
const Vehicle kVehicles[] = {
    {"BomberHelicopter", 0x91f388, 0x850938, 0x54cb40, {0x68, 0x88, 0xf3, 0x91, 0x00, 0xe8, 0x4b, 0x72, 0x1a, 0x00}},
    {"SuperAirstrike", 0x91fc28, 0x850cf0, 0x589870, {0x68, 0x28, 0xfc, 0x91, 0x00, 0xe8, 0x1b, 0xa5, 0x16, 0x00}},
};
constexpr size_t kVehicleCount = sizeof kVehicles / sizeof kVehicles[0];
std::deque<std::string> g_vehicleNames;  // what patched variables point at: elements never move or die, so a pointer into one stays valid
uint32_t g_vehicleOrig[kVehicleCount] = {};  // the vanilla pointer while patched, else 0
}  // namespace

bool SetVehicleMesh(const char* vehicle, const char* name, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    size_t i = 0;
    while (i < kVehicleCount && (!vehicle || strcmp(kVehicles[i].key, vehicle) != 0)) ++i;
    if (i == kVehicleCount) return fail("not a known vehicle");
    if (!name || !*name || strlen(name) > 96) return fail("bad mesh name");
    const Vehicle& v = kVehicles[i];
    if (!game::IsKnownBuild() || !mem::Expect(v.push, v.bytes))
        return fail("the code that creates this vehicle's mesh is not build #1077's");
    const uint32_t cur = Rd<uint32_t>(v.var);
    if (!g_vehicleOrig[i] && (cur != v.vanilla || ReadCString(cur, 32) != v.key))
        return fail("the vehicle's mesh name is not the vanilla one");
    // A name the variable may still point at is never overwritten (a failed Put below would leave it dangling): each
    // distinct name gets its own stable string, reused when set again.
    auto at = std::find(g_vehicleNames.begin(), g_vehicleNames.end(), name);
    if (at == g_vehicleNames.end()) {
        g_vehicleNames.emplace_back(name);
        at = g_vehicleNames.end() - 1;
    }
    const uint32_t p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(at->c_str()));
    if (!mem::Put<uint32_t>(v.var, p)) return fail("writing the mesh name failed");
    if (!g_vehicleOrig[i]) g_vehicleOrig[i] = v.vanilla;
    return true;
}

void ClearVehicleMeshes() {
    for (size_t i = 0; i < kVehicleCount; ++i) {
        if (!g_vehicleOrig[i]) continue;
        mem::Put<uint32_t>(kVehicles[i].var, g_vehicleOrig[i]);
        g_vehicleOrig[i] = 0;
    }
}

std::vector<HookState> Hooks() {
    std::vector<HookState> v;
    for (auto& t : g_hooks) {
        const bool created = t.mid ? static_cast<bool>(*t.mid) : static_cast<bool>(*t.in);
        const bool enabled = t.mid ? (*t.mid && t.mid->enabled()) : (*t.in && t.in->enabled());
        v.push_back({t.name, t.site, created, t.wanted, enabled});
    }
    return v;
}
}  // namespace melange::weapons::engine
