#include "levels/engine.h"

#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>

#include "assets/searchpath.h"
#include "melange/bus.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "melange/sim.h"
#include "weapons/engine.h"

namespace melange::levels::engine {
namespace {
namespace weng = weapons::engine;

struct Site {
    uintptr_t addr;
    std::initializer_list<int> bytes;
};
const Site kFns[] = {
    {kLoadDataBank, {0x6a, 0xff, 0x68, 0xd8, 0x26, 0x7d, 0x00}},
    {kGetFloat, {0xe8, 0x28, 0xe3, 0x12, 0x00, 0x8b, 0x08}},
    {kSetFloat, {0x6a, 0xff, 0x68, 0x38, 0x93, 0x7c, 0x00}},
    {kGetInt, {0xe8, 0x88, 0xe3, 0x12, 0x00, 0x8b, 0x08}},
    {kSetUp, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0x98}},
    {kPickerReject, {0x8b, 0x4c, 0x24, 0x34, 0x5f, 0x5e, 0x5d, 0xb0, 0x01}},
};
constexpr std::initializer_list<int> kLevelNameBytes = {0x68, 0x80, 0x8b, 0x8b, 0x00};
constexpr std::initializer_list<int> kPickerKeepBytes = {0x8b, 0x43, 0x30, 0x8b, 0x74, 0x24, 0x48};
constexpr std::initializer_list<int> kPoolEntryBytes = {0x8b, 0x46, 0x30, 0x83, 0xf8, 0x03};
const Site kExtra[] = {
    {kPoolSkip, {0x85, 0xdb, 0x74, 0x06, 0x8b, 0x03, 0x53}},
    {kMsgAlloc, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x10}},
    {kTwoStringMsgInit, {0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc}},
    {kMsgPost, {0x55, 0x8b, 0xec, 0x51, 0x51, 0x83, 0x3d}},
    {kSetString, {0x6a, 0xff, 0x68, 0x38, 0x93, 0x7c, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x64, 0x89, 0x25,
                  0x00, 0x00, 0x00, 0x00, 0x51, 0x56, 0xc7}},
    {kClearDataBank, {0xa1, 0xe0, 0x26, 0x96, 0x00, 0x85, 0xc0, 0x74, 0x05, 0x83, 0xc0, 0x14}},
    {kPoolRebuild, {0x55, 0x8b, 0xec, 0x81, 0xec, 0x20, 0x01, 0x00, 0x00, 0x53, 0x56, 0x8b, 0xf1}},
};

// Where FlowControlService::Update reads the fields ReadFrontend reports: the global, the state switch, the idle
// deadline against the clock with the attract-allowed bit, and the attract-running bit.
const Site kFcs[] = {
    {0x4f11ba, {0xa1, 0x98, 0xa2, 0x95, 0x00}},
    {0x4f15b0, {0x8b, 0x86, 0x38, 0x01, 0x00, 0x00, 0x3b, 0x86, 0x3c, 0x01}},
    {0x4f19a2, {0xf6, 0x86, 0x59, 0x01, 0x00, 0x00, 0x01, 0x0f, 0x84}},
    {0x4f19af, {0xa1, 0x30, 0xd0, 0x96, 0x00, 0x8b, 0x40, 0x38, 0x3b, 0x86, 0x50, 0x01, 0x00, 0x00}},
    {0x4f1a31, {0x80, 0x8e, 0x60, 0x01, 0x00, 0x00, 0x01}},
};

bool ExtraOk(uintptr_t addr) {
    if (!game::IsKnownBuild()) return false;
    // The bus module hooks Post after checking these same bytes; a post then goes through its hook.
    if (addr == kMsgPost && bus::Installed()) return true;
    for (auto& s : kExtra)
        if (s.addr == addr) return mem::Expect(s.addr, s.bytes);
    return false;
}

bool FnOk(uintptr_t addr) {
    if (!game::IsKnownBuild()) return false;
    for (auto& s : kFns)
        if (s.addr == addr) return mem::Expect(s.addr, s.bytes);
    return false;
}

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

int RawLoadDataBank(const char* name, uint32_t section, uint32_t flags) {
    __try {
        return reinterpret_cast<int(__cdecl*)(const char*, uint32_t, uint32_t)>(kLoadDataBank)(name, section, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int RawGetFloat(const char* name, float* v) {
    __try {
        return reinterpret_cast<int(__cdecl*)(const char**, float*)>(kGetFloat)(&name, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int RawSetFloat(const char* name, float v) {
    __try {
        return reinterpret_cast<int(__cdecl*)(const char**, float)>(kSetFloat)(&name, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int RawGetInt(const char* name, int* v) {
    __try {
        return reinterpret_cast<int(__cdecl*)(const char**, int*)>(kGetInt)(&name, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const char* RawEntryName(uintptr_t entry) {
    __try {
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>(entry);
        return reinterpret_cast<const char*(__stdcall*)(uintptr_t)>(*reinterpret_cast<uintptr_t*>(vt + 0x18))(entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool IsDetailsClass(uintptr_t c) {
    uintptr_t cls = weng::ClassOf(c);
    for (int depth = 0; cls && depth < 8; ++depth) {
        if (weng::ClassName(cls) == "WXFE_LevelDetails") return true;
        const uintptr_t parent = weng::ClassParent(cls);
        if (parent == cls) break;
        cls = parent;
    }
    return false;
}

DecideFn g_decide = nullptr;
void* g_decideUser = nullptr;
SafetyHookMid g_levelHook;
char g_override[128] = {};

using Clock = std::chrono::steady_clock;
std::atomic<int64_t> g_loadingSinceMs{0};
constexpr int64_t kLoadingMaxMs = 90000;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void OnLevelName(safetyhook::Context& c) {
    g_loadingSinceMs = NowMs();
    if (!g_decide) return;
    const uintptr_t slot = c.esp + 0x20;
    const char* cur = reinterpret_cast<const char*>(Rd<uintptr_t>(slot));
    char name[128] = {};
    if (cur) mem::SafeRead(reinterpret_cast<uintptr_t>(cur), name, sizeof name - 1);
    const char* want = g_decide(name, g_decideUser);
    if (!want || !*want || std::strcmp(want, name) == 0) return;
    strncpy_s(g_override, want, _TRUNCATE);
    *reinterpret_cast<const char**>(slot) = g_override;
}

KeepFn g_keep = nullptr;
SafetyHookMid g_pickerHook;

void OnPickerKeep(safetyhook::Context& c) {
    if (!g_keep) return;
    const char* key = RawEntryName(c.ebp);
    if (!key) return;
    const uint32_t request = Rd<uint32_t>(Rd<uintptr_t>(c.esp + 0x48));
    if (!g_keep(key, request)) c.eip = kPickerReject;
}

KeepFn g_poolKeep = nullptr;
SafetyHookMid g_poolHook;

void OnPoolEntry(safetyhook::Context& c) {
    if (!g_poolKeep) return;
    const char* key = RawEntryName(c.ebx);
    if (!key) return;
    if (!g_poolKeep(key, Rd<uint32_t>(c.esi + 0x30))) c.eip = kPoolSkip;
}

bool RawPostTwoStrings(uint16_t id, const char* a, const char* b) {
    __try {
        const uintptr_t factory = *reinterpret_cast<uintptr_t*>(kMsgFactory);
        if (!factory) return false;
        const uintptr_t msg = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uint32_t)>(kMsgAlloc)(factory, 0x10);
        if (!msg) return false;
        reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uint32_t, const char*, const char*)>(kTwoStringMsgInit)(msg, id, a, b);
        reinterpret_cast<int(__cdecl*)(uintptr_t)>(kMsgPost)(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool RawSetString(const char* name, const char* v) {
    __try {
        return reinterpret_cast<int(__cdecl*)(const char**, const char*)>(kSetString)(&name, v) >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RawClearDataBank(uint32_t section) {
    __try {
        reinterpret_cast<void(__cdecl*)(uint32_t)>(kClearDataBank)(section);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool RawRebuild(uintptr_t ms) {
    __try {
        reinterpret_cast<void(__thiscall*)(uintptr_t)>(kPoolRebuild)(ms);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool FcsOk() {
    static const bool ok = [] {
        if (!game::IsKnownBuild()) return false;
        for (auto& s : kFcs)
            if (!mem::Expect(s.addr, s.bytes)) return false;
        return true;
    }();
    return ok;
}

struct StartObserver {
    int handle;
    StartGameFn fn;
    void* user;
};
std::mutex g_startMx;
std::vector<StartObserver> g_startObs;
int g_startNext = 1;
bus::SubId g_startSub = 0;

void OnStartGameMsg(const bus::MessageView& m, void*) {
    uintptr_t p = 0;
    char text[64] = {};
    if (m.Get(8, p) && p) mem::SafeRead(p, text, sizeof text - 1);
    std::vector<StartObserver> obs;
    {
        std::lock_guard lk(g_startMx);
        obs = g_startObs;
    }
    for (auto& o : obs) o.fn(text, o.user);
}

bool RegisterSites() {
    static bool done = false;
    if (!done) {
        weng::AddHookSite(kLevelName, kLevelNameBytes);
        weng::AddHookSite(kPickerKeep, kPickerKeepBytes);
        weng::AddHookSite(kPoolEntry, kPoolEntryBytes);
        done = true;
    }
    return true;
}
}  // namespace

bool SitesOk() {
    RegisterSites();
    bool ok = game::IsKnownBuild();
    for (auto& s : kFns)
        if (!mem::Expect(s.addr, s.bytes)) {
            LOG_ERROR("[levels] code bytes at %08x differ from build #1077", static_cast<unsigned>(s.addr));
            ok = false;
        }
    for (uintptr_t site : {kLevelName, kPickerKeep})
        if (!weng::SiteIntact(site)) {
            LOG_ERROR("[levels] code bytes at %08x differ from build #1077", static_cast<unsigned>(site));
            ok = false;
        }
    return ok;
}

bool LevelNameIntact(const char* s) {
    if (!s || !*s) return false;
    const size_t n = std::strlen(s);
    if (n > 200) return false;
    const char* dot = std::strchr(s, '.');
    if (dot) {
        const char* last = std::strrchr(s, '.');
        if (dot != last) return false;
        if (std::strpbrk(dot, "/\\")) return false;
        if (_stricmp(dot, ".xom") != 0) return false;
    }
    for (const char* p = s; *p; ++p)
        if (static_cast<unsigned char>(*p) < 0x20 || static_cast<unsigned char>(*p) >= 0x7f || *p == ':') return false;
    return s[0] != '/' && s[0] != '\\';
}

int LoadDataBank(const char* gameRelPath, uint32_t section) {
    if (!LevelNameIntact(gameRelPath)) {
        LOG_WARN("[levels] LoadDataBank refused '%s': a bank path may hold one '.', before the .xom extension",
                 gameRelPath ? gameRelPath : "");
        return -1;
    }
    if (section > 34 || !FnOk(kLoadDataBank) || !weng::Drm()) return -1;
    return RawLoadDataBank(gameRelPath, section, 0x20);
}

bool AddString(const char* name, const char* value, uint32_t section) {
    if (!name || !*name || !value) return false;
    return weng::AddString(name, value, section, 1) >= 0;
}

bool GetFloat(const char* name, float* v) {
    if (!name || !*name || !v || !FnOk(kGetFloat)) return false;
    return RawGetFloat(name, v) >= 0;
}

bool SetFloat(const char* name, float v) {
    if (!name || !*name || !FnOk(kSetFloat)) return false;
    return RawSetFloat(name, v) >= 0;
}

const char* CurrentLevelKey() {
    static std::string key;
    key.clear();
    if (!weng::TextOf("WXD.Level.Current", &key)) key.clear();
    return key.c_str();
}

bool AtFrontend() {
    if (!FnOk(kGetInt) || sim::InMatch()) return false;
    int scope = -1;
    return RawGetInt("Game.Scope", &scope) >= 0 && scope == 0;
}

bool AddRoot(const char* gameRelDir) { return assets::searchpath::Add(gameRelDir); }

bool LevelDetails(const char* key, Details* out) {
    const uintptr_t c = weng::Lookup(key);
    if (!c || !IsDetailsClass(c)) return false;
    if (out) {
        out->script = weng::XStringValue(c + 0x20);
        out->file = weng::XStringValue(c + 0x24);
        out->levelType = Rd<int>(c + 0x30);
        out->lock = weng::XStringValue(c + 0x34);
        out->themeType = Rd<int>(c + 0x38);
        out->previewType = Rd<int>(c + 0x3c);
        out->frontendName = weng::XStringValue(c + 0x14);
    }
    return true;
}

bool InstallLevelHook(DecideFn decide, void* user) {
    RegisterSites();
    g_decide = decide;
    g_decideUser = user;
    if (!weng::Mid(g_levelHook, kLevelName, &OnLevelName, "level name")) return false;
    static bool tracked = false;
    if (!tracked) {
        tracked = true;
        events::Subscribe(events::Event::Frame, [] {
            const int64_t t = g_loadingSinceMs.load();
            if (t && (sim::InMatch() || NowMs() - t > kLoadingMaxMs)) g_loadingSinceMs = 0;
        });
    }
    weng::Enable(g_levelHook, true);
    return true;
}

void EnableLevelHook(bool on) {
    if (g_levelHook) weng::Enable(g_levelHook, on);
}

bool LevelHookEnabled() { return g_levelHook && g_levelHook.enabled(); }

void CurrentLevelHook(DecideFn* fn, void** user) {
    if (fn) *fn = g_decide;
    if (user) *user = g_decideUser;
}

bool InstallPickerHook(KeepFn keep) {
    RegisterSites();
    g_keep = keep;
    if (!FnOk(kPickerReject)) return false;
    if (!weng::Mid(g_pickerHook, kPickerKeep, &OnPickerKeep, "picker keep")) return false;
    weng::Enable(g_pickerHook, true);
    return true;
}

void EnablePickerHook(bool on) {
    if (g_pickerHook) weng::Enable(g_pickerHook, on);
}

bool PickerHookEnabled() { return g_pickerHook && g_pickerHook.enabled(); }

KeepFn CurrentPickerKeep() { return g_keep; }

bool InstallPoolHook(KeepFn keep) {
    RegisterSites();
    g_poolKeep = keep;
    if (!ExtraOk(kPoolSkip)) return false;
    if (!weng::Mid(g_poolHook, kPoolEntry, &OnPoolEntry, "random pool")) return false;
    weng::Enable(g_poolHook, true);
    return true;
}

void EnablePoolHook(bool on) {
    if (g_poolHook) weng::Enable(g_poolHook, on);
}

bool PoolHookEnabled() { return g_poolHook && g_poolHook.enabled(); }

std::vector<std::string> PoolKeys() { return PoolKeysAt(0x2c); }

std::vector<std::string> PoolKeysAt(uint32_t offset) {
    std::vector<std::string> out;
    if (!game::IsKnownBuild() || offset > 0x200 || offset % 4) return out;
    const uintptr_t ms = Rd<uintptr_t>(kMissionService);
    if (!ms) return out;
    const uintptr_t b = Rd<uintptr_t>(ms + offset), e = Rd<uintptr_t>(ms + offset + 4);
    if (!b || e < b || e - b > 4 * 1024) return out;
    for (uintptr_t p = b; p < e; p += 4) {
        std::string k = weng::ReadCString(Rd<uintptr_t>(p), 80);
        if (!k.empty()) out.push_back(std::move(k));
    }
    return out;
}

bool PostDataResource(const char* name, const char* value) {
    if (!name || !*name || !value) return false;
    if (!ExtraOk(kMsgAlloc) || !ExtraOk(kTwoStringMsgInit) || !ExtraOk(kMsgPost)) return false;
    const auto id = bus::IdOf("WXMsg.SetDataResource");
    if (id == bus::kInvalidId) return false;
    // The message keeps the pointers, not copies: the text lives for the rest of the process.
    static std::deque<std::string> texts;
    if (texts.size() > 64) return false;
    texts.emplace_back(name);
    const char* a = texts.back().c_str();
    texts.emplace_back(value);
    const char* b = texts.back().c_str();
    return RawPostTwoStrings(id, a, b);
}

bool SetString(const char* name, const char* v) {
    if (!name || !*name || !v || !ExtraOk(kSetString)) return false;
    return RawSetString(name, v);
}

bool GetString(const char* name, std::string* out) {
    if (!name || !*name) return false;
    std::string v;
    if (!weng::TextOf(name, &v)) return false;
    if (out) *out = std::move(v);
    return true;
}

bool ClearDataBank(uint32_t section) {
    if (section != 12) {
        LOG_WARN("[levels] ClearDataBank(%u) refused: only section 12 may be cleared", section);
        return false;
    }
    if (!ExtraOk(kClearDataBank) || !AtFrontend()) return false;
    return RawClearDataBank(section);
}

bool RebuildPools() {
    if (!ExtraOk(kPoolRebuild) || !AtFrontend()) return false;
    const uintptr_t ms = Rd<uintptr_t>(kMissionService);
    return ms && RawRebuild(ms);
}

FrontendState ReadFrontend() {
    FrontendState f{};
    if (!FcsOk()) return f;
    const uintptr_t fcs = Rd<uintptr_t>(kFrontendControl);
    if (!fcs) return f;
    uint8_t b159 = 0, b160 = 0;
    if (!mem::SafeRead(fcs + 0x138, &f.state, 4) || !mem::SafeRead(fcs + 0x150, &f.idleDeadline, 4) ||
        !mem::SafeRead(fcs + 0x159, &b159, 1) || !mem::SafeRead(fcs + 0x160, &b160, 1))
        return FrontendState{};
    f.attractAllowed = (b159 & 1) != 0;
    f.attractRunning = (b160 & 1) != 0;
    f.valid = true;
    return f;
}

bool Loading() { return g_loadingSinceMs.load() != 0; }

int OnStartGame(StartGameFn fn, void* user) {
    if (!fn) return 0;
    std::lock_guard lk(g_startMx);
    if (!g_startSub) g_startSub = bus::SubscribeName("WXMsg.StartGame", bus::Path::Post, &OnStartGameMsg, nullptr);
    if (!g_startSub) return 0;
    g_startObs.push_back({g_startNext, fn, user});
    return g_startNext++;
}

void RemoveOnStartGame(int handle) {
    std::lock_guard lk(g_startMx);
    std::erase_if(g_startObs, [handle](const StartObserver& o) { return o.handle == handle; });
}
}  // namespace melange::levels::engine
