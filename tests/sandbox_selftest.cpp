// Offline self-test for the Sandbox (client Lua VM): environments, escapes, limits, faults, hot reload, events, timers,
// config, storage, console eval, wum.unsafe, panels and the API reference. Thumper, the overlay, draw, render, postfx,
// config, jlog and the game are replaced by fakes below. Exit code 0 = all passed.
#include <windows.h>

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/game.h"
#include "core/log.h"
#include "lua/sandbox_core.h"
#include "lua/sandbox_internal.h"
#include "melange/draw.h"
#include "melange/gamestate.h"
#include "melange/graphics.h"
#include "melange/jlog.h"
#include "melange/lua.h"
#include "melange/mods.h"
#include "melange/overlay.h"
#include "melange/postfx.h"
#include "melange/render.h"
#include "melange/shaders.h"

namespace fake {
struct Record {
    std::string cat, msg, fields;
};
std::vector<Record> g_recs;
std::map<std::string, std::string> g_config;
double g_clock = 1000.0;
std::set<std::string> g_engineSubs;
std::wstring g_root, g_modsDir, g_dataDir;
struct Mod {
    std::string id, name, version;
    std::wstring dir;
    bool unsafe = false, granted = false, enabled = true;
    int order = 0;
};
std::vector<Mod> g_mods;
struct PanelReg {
    int handle;
    std::string id;
    melange::overlay::DrawFn fn;
    void* user;
};
std::vector<PanelReg> g_panels;
struct MenuReg {
    std::string path;
    melange::overlay::ActionFn fn;
    void* user;
    melange::overlay::CheckedFn checked;
};
std::vector<MenuReg> g_menus;
int g_nextHandle = 1, g_textures = 0, g_drawCbs = 0, g_hotkeys = 0;
}  // namespace fake

// ---------------------------------------------------------------- link-time fakes
namespace melange {
namespace log {
void Write(const char*, const char* fmt, ...) {
    (void)fmt;
}
}  // namespace log
namespace game {
const ExeInfo& Exe() {
    static ExeInfo e = [] {
        ExeInfo x;
        x.build = "selftest";
        return x;
    }();
    return e;
}
uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
const std::wstring& DataDir() { return fake::g_dataDir; }
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace game
namespace config {
std::string GetString(const char* section, const char* key, const char* def) {
    auto it = fake::g_config.find(std::string(section) + "/" + key);
    return it == fake::g_config.end() ? def : it->second;
}
void SetString(const char* section, const char* key, const char* value) { fake::g_config[std::string(section) + "/" + key] = value; }
}  // namespace config
namespace jlog {
struct Rec::Impl {
    fake::Record r;
    bool emitted = false;
};
Rec::Rec(std::string_view category, Level, std::string_view msg) : p_(new Impl) {
    p_->r.cat = category;
    p_->r.msg = msg;
}
Rec& Rec::Int(const char* k, int64_t v) { return Str(k, std::to_string(v)); }
Rec& Rec::Uint(const char* k, uint64_t v) { return Str(k, std::to_string(v)); }
Rec& Rec::Hex(const char* k, uint64_t v) { return Str(k, std::to_string(v)); }
Rec& Rec::Float(const char* k, double v) { return Str(k, std::to_string(v)); }
Rec& Rec::Bool(const char* k, bool v) { return Str(k, v ? "true" : "false"); }
Rec& Rec::Vec3(const char* k, const float*) { return Str(k, "vec3"); }
Rec& Rec::Raw(const char* k, std::string_view json) { return Str(k, json); }
Rec& Rec::Str(const char* k, std::string_view v) {
    p_->r.fields += std::string(k) + "=" + std::string(v) + ";";
    return *this;
}
void Rec::Emit() {
    if (p_->emitted) return;
    p_->emitted = true;
    fake::g_recs.push_back(p_->r);
}
Rec::~Rec() {
    Emit();
    delete p_;
}
}  // namespace jlog
namespace mods {
int List(ModInfo* out, int max) {
    int n = 0;
    for (const fake::Mod& m : fake::g_mods) {
        if (n < max && out) {
            ModInfo& i = out[n];
            i = {};
            i.id = m.id.c_str();
            i.name = m.name.c_str();
            i.version = m.version.c_str();
            i.dir = m.dir.c_str();
            i.state = m.enabled ? State::Enabled : State::Disabled;
            i.hasClient = true;
            i.unsafe = m.unsafe;
            i.unsafeGranted = m.granted;
            i.order = m.order;
        }
        ++n;
    }
    return n;
}
bool Find(const char* id, ModInfo* out) {
    std::vector<ModInfo> all(fake::g_mods.size() + 1);
    const int n = List(all.data(), static_cast<int>(all.size()));
    for (int i = 0; i < n; ++i)
        if (strcmp(all[static_cast<size_t>(i)].id, id) == 0) {
            *out = all[static_cast<size_t>(i)];
            return true;
        }
    return false;
}
}  // namespace mods
namespace overlay {
int AddPanel(const char* id, const char*, DrawFn fn, void* user, uint32_t) {
    for (const auto& p : fake::g_panels)
        if (p.id == id) return 0;
    fake::g_panels.push_back({fake::g_nextHandle, id, fn, user});
    return fake::g_nextHandle++;
}
void RemovePanel(int handle) {
    for (auto it = fake::g_panels.begin(); it != fake::g_panels.end(); ++it)
        if (it->handle == handle) {
            fake::g_panels.erase(it);
            return;
        }
}
int AddToggleMenuItem(const char* path, ActionFn fn, void* user, CheckedFn checked, const char*) {
    fake::g_menus.push_back({path, fn, user, checked});
    return fake::g_nextHandle++;
}
int AddMenuItem(const char* path, ActionFn fn, void* user, const char* shortcut) {
    return AddToggleMenuItem(path, fn, user, nullptr, shortcut);
}
int AddHotkey(uint8_t, uint8_t, ActionFn, void*) { return ++fake::g_hotkeys; }
bool ParseHotkey(const char* text, uint8_t* dik, uint8_t* mods) {
    *dik = 0x23;
    *mods = kCtrl;
    return text && *text;
}
}  // namespace overlay
namespace draw {
void Line(const float*, const float*, Rgba, float, uint32_t, int) {}
void Box(const float*, const float*, Rgba, float, uint32_t, int) {}
void Sphere(const float*, float, Rgba, float, uint32_t, int) {}
void Axes(const float*, float, float, uint32_t, int) {}
void Quad(const float (*)[3], Rgba, uint32_t, int) {}
void Text(const float*, const char*, Rgba, float, uint32_t, int) {}
void HudLine(float, float, float, float, Rgba, float, int) {}
void HudRect(float, float, float, float, Rgba, bool, float, int) {}
Rgba g_lastHudColor = 0;
void HudText(float, float, const char*, Rgba c, float, int) { g_lastHudColor = c; }
void HudImage(float, float, float, float, unsigned, Rgba, int) {}
render::Stage g_stage = render::Stage::Count;
uint64_t g_frame = 1;
std::vector<Sprite> g_sprites;
size_t g_spriteCap = kMaxSpritesPerStage;
bool DrawSprite(const Sprite& s) {
    if (g_stage != render::Stage::World && g_stage != render::Stage::WorldLate) return false;
    if (g_sprites.size() >= g_spriteCap) return false;
    g_sprites.push_back(s);
    return true;
}
render::Stage CurrentStage() { return g_stage; }
uint64_t FrameSerial() { return g_frame; }
unsigned g_nextTexture = 100;
unsigned LoadTexture(const wchar_t*) {
    ++fake::g_textures;
    return g_nextTexture++;
}
void FreeTexture(unsigned) { --fake::g_textures; }
int AddDrawCallback(render::Stage, DrawFn, void*, int) {
    ++fake::g_drawCbs;
    return fake::g_nextHandle++;
}
void RemoveDrawCallback(int) { --fake::g_drawCbs; }
}  // namespace draw
namespace render {
bool GetCamera(Camera*) { return false; }
bool WorldToScreen(const float*, float*, float*, float*) { return false; }
void WindowSize(int* w, int* h) { *w = 1024, *h = 768; }
Timing GetTiming() { return {}; }
}  // namespace render
namespace postfx {
size_t ListEffects(EffectInfo* out, size_t max) {
    if (max < 2) return 0;
    out[0] = {"own/glow", "Glow", render::Stage::Final, 0, true, false, 0, 0};
    out[1] = {"other/blur", "Blur", render::Stage::PostWorld, 0, false, false, 0, 0};
    return 2;
}
bool SetEnabled(const char*, bool) { return true; }
bool SetParam(const char*, const char*, const float*, int) { return true; }
bool SetParamTransient(const char*, const char*, const float*, int) { return true; }
bool GetParam(const char*, const char*, float* v, int n) {
    for (int i = 0; i < n; ++i) v[i] = 0.5f;
    return true;
}
}  // namespace postfx
namespace graphics {
std::string g_lastMod;
int g_lastSize = -2;
bool SetShadowMapRequest(const char* mod, int size) {
    g_lastMod = mod;
    g_lastSize = size;
    return true;
}
ShadowMapInfo GetShadowMapInfo() { return {2048, 2048, 1024, true, true}; }
int g_lastSamples = -2;
bool SetSupersampleRequest(const char* mod, int samples) {
    g_lastMod = mod;
    g_lastSamples = samples;
    return true;
}
SupersampleInfo GetSupersampleInfo() { return {2, 2, 4, 3840, 2160, false, true, true}; }
}  // namespace graphics
namespace shaders {
bool SetOwnParam(const char* owner, const char*, const char*, const char* param, const float*, int) {
    return strcmp(owner, "esc") == 0 && strcmp(param, "softness") == 0;
}
std::string g_glslEntry;
bool g_glslOn = true;
bool SetOwnGlslEnabled(const char* owner, const char*, const char* entry, bool on) {
    if (strcmp(owner, "esc") != 0) return false;
    g_glslEntry = entry;
    g_glslOn = on;
    return true;
}
}  // namespace shaders
namespace gamestate {
Snapshot g_snapshot{};
bool g_readable = false;
bool Read(Snapshot* s) {
    if (!g_readable) return false;
    *s = g_snapshot;
    return true;
}
LandRayResult g_rayResult = LandRayResult::Unavailable;
LandHit g_rayHit{};
Vec3 g_rayA{}, g_rayB{};
int g_rayCalls = 0;
LandRayResult LandRay(const Vec3& a, const Vec3& b, LandHit* out) {
    ++g_rayCalls;
    g_rayA = a;
    g_rayB = b;
    if (g_rayResult == LandRayResult::Hit) *out = g_rayHit;
    return g_rayResult;
}
}  // namespace gamestate
namespace sandbox {
double NowSeconds() { return fake::g_clock; }
void EngineSubscribe(const std::string& name, bool on) {
    if (on) fake::g_engineSubs.insert(name);
    else fake::g_engineSubs.erase(name);
}
}  // namespace sandbox
}  // namespace melange

// ---------------------------------------------------------------- helpers
using namespace melange;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

void ExpectEq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) printf("  %s: got \"%s\", want \"%s\"\n", what.c_str(), got.c_str(), want.c_str());
    Expect(got == want, what);
}

void WriteText(const std::wstring& path, const std::string& text) {
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    std::ofstream f(path, std::ios::binary);
    f << text;
}

std::string ReadText(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::wstring W(const std::string& s) { return sandbox::Widen(s); }

void SetMod(const std::string& id, const std::string& lua, const std::string& extraManifest = "", bool unsafe = false,
            bool granted = false) {
    const std::wstring dir = fake::g_modsDir + L"\\" + W(id);
    CreateDirectoryW(dir.c_str(), nullptr);
    WriteText(dir + L"\\spice.json", "{\"id\":\"" + id + "\",\"version\":\"1.0.0\",\"entry\":{\"client\":\"client/init.lua\"}" +
                                         extraManifest + "}");
    WriteText(dir + L"\\client\\init.lua", lua);
    for (fake::Mod& m : fake::g_mods)
        if (m.id == id) return;
    fake::Mod m;
    m.id = m.name = id;
    m.version = "1.0.0";
    m.dir = dir;
    m.unsafe = unsafe;
    m.granted = granted;
    m.order = static_cast<int>(fake::g_mods.size());
    fake::g_mods.push_back(m);
}

std::string Eval(const char* mod, const std::string& code) {
    const sandbox::EvalOut r = sandbox::Eval(mod, code);
    return (r.ok ? "" : "ERR:") + r.text;
}

void Frames(int n, double dt = 0.02) {
    for (int i = 0; i < n; ++i) {
        fake::g_clock += dt;
        sandbox::Frame();
    }
}

sandbox::ModStatus StatusOf(const char* id) {
    sandbox::ModStatus s;
    sandbox::Status(id, &s);
    return s;
}

bool HasRecord(const std::string& cat, const std::string& msg, const std::string& fieldPart = "") {
    for (const auto& r : fake::g_recs)
        if (r.cat == cat && r.msg.rfind(msg, 0) == 0 && r.fields.find(fieldPart) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------- test library for AddLibrary and wum.unsafe
uint32_t g_buffer[4] = {0x12345678, 0, 0, 0};
const char g_text[] = "melange";
int __cdecl NativeAdd(int a, int b) { return a + b; }
int __stdcall NativeAddStd(int a, int b) { return a * 10 + b; }
float __cdecl NativeHalf(float v) { return v / 2; }
struct Obj {
    int v = 100;
    int Plus(int x) { return v + x; }
};
Obj g_obj;

int ProbeWho(lua_State* L) {
    const char* id = lua::CurrentMod();
    lua_pushstring(L, id ? id : "(none)");
    return 1;
}

int OpenProbe(lua_State* L) {
    lua_newtable(L);
    auto put = [&](const char* k, uintptr_t v) {
        lua_pushinteger(L, static_cast<lua_Integer>(v));
        lua_setfield(L, -2, k);
    };
    put("buf", reinterpret_cast<uintptr_t>(g_buffer));
    put("str", reinterpret_cast<uintptr_t>(g_text));
    put("add", reinterpret_cast<uintptr_t>(&NativeAdd));
    put("addStd", reinterpret_cast<uintptr_t>(&NativeAddStd));
    put("half", reinterpret_cast<uintptr_t>(&NativeHalf));
    auto mfp = &Obj::Plus;
    uintptr_t raw = 0;
    memcpy(&raw, &mfp, sizeof(raw));
    put("plus", raw);
    put("obj", reinterpret_cast<uintptr_t>(&g_obj));
    return 1;
}

int OpenWho(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, &ProbeWho);
    lua_setfield(L, -2, "who");
    return 1;
}

// ---------------------------------------------------------------- tests
void TestEscapes() {
    SetMod("esc", R"lua(
local fails = {}
local function fail(n) fails[#fails + 1] = n end
local function must_fail(n, f) if pcall(f) then fail(n) end end
for _, n in ipairs({"load", "loadstring", "dofile", "loadfile", "require", "io", "debug", "package", "module"}) do
  if _ENV[n] ~= nil then fail(n) end
end
if string.dump ~= nil then fail("string.dump") end
if ("x").dump ~= nil then fail("method dump") end
for _, n in ipairs({"execute", "getenv", "exit", "remove", "rename", "tmpname", "setlocale"}) do
  if os[n] ~= nil then fail("os." .. n) end
end
if type(os.time()) ~= "number" or type(os.clock()) ~= "number" or type(os.date("%Y")) ~= "string" then fail("os time") end
must_fail("wum write", function() wum.x = 1 end)
must_fail("wum.log write", function() wum.log.info = nil end)
must_fail("string write", function() string.upper = nil end)
must_fail("rawset string", function() rawset(string, "upper", print) end)
must_fail("rawset wum", function() rawset(wum, "x", 1) end)
must_fail("setmetatable string", function() setmetatable(string, {}) end)
must_fail("setmetatable env", function() setmetatable(_ENV, nil) end)
must_fail("__gc", function() setmetatable({}, {__gc = function() end}) end)
must_fail("collectgarbage stop", function() collectgarbage("stop") end)
must_fail("table.insert frozen", function() table.insert(string, 1) end)
must_fail("pairs leak", function() local _, t = pairs(string); rawset(t, "upper", 1) end)
must_fail("pairs iterator", function() local f = pairs(math); local t = select(2, f(nil, nil)); rawset(t, 1, 1) end)
if getmetatable("") ~= "locked" then fail("string metatable") end
if getmetatable(_ENV) ~= "locked" then fail("env metatable") end
if getmetatable(wum) ~= "locked" then fail("wum metatable") end
if _G ~= _ENV then fail("_G") end
if ("abc"):upper() ~= "ABC" then fail("string methods") end
local n = 0 for k in pairs(math) do n = n + 1 end
if n < 20 then fail("pairs over math") end
if mods ~= nil then fail("mods visible to mods") end
if collectgarbage("count") <= 0 then fail("count") end
secret = 42
escape_fails = table.concat(fails, ",")
)lua");
    Expect(sandbox::LoadMod("esc"), "esc loads");
    ExpectEq(Eval("esc", "return escape_fails"), "", "every escape attempt fails");
    SetMod("other", "function peek() return secret end\n");
    Expect(sandbox::LoadMod("other"), "other loads");
    ExpectEq(Eval("other", "return peek()"), "nil", "another mod's globals are not visible");
    ExpectEq(Eval("other", "return wum.mod.id"), "other", "wum.mod is per environment");
    ExpectEq(Eval("esc", "return wum.mod.id"), "esc", "wum.mod of esc");
    ExpectEq(Eval(nullptr, "return mods.esc.secret"), "42", "the console reaches mod environments");
    ExpectEq(Eval(nullptr, "return mods.nope"), "nil", "unknown mod in the console");

    SetMod("bytecode", std::string("\x1bLua\x54\x00", 6) + "junk");
    Expect(!sandbox::LoadMod("bytecode"), "binary chunk refused");
    Expect(StatusOf("bytecode").error.find("binary") != std::string::npos, "binary chunk error: " + StatusOf("bytecode").error);
}

void TestBudget() {
    SetMod("toploop", "while true do end\n");
    Expect(!sandbox::LoadMod("toploop"), "top-level loop stopped");
    Expect(StatusOf("toploop").error.find("instruction budget") != std::string::npos, "top-level loop error");
    Expect(!StatusOf("toploop").loaded, "top-level loop not loaded");

    SetMod("loop", "wum.timers.every(0, function() while true do end end)\n");
    Expect(sandbox::LoadMod("loop"), "loop loads");
    Frames(1);
    Expect(StatusOf("loop").faults == 1, "loop faulted once");
    Frames(4);
    Expect(StatusOf("loop").faults == 3, "loop faulted 3 times, then stopped: " + std::to_string(StatusOf("loop").faults));
    Expect(StatusOf("loop").disabledCallbacks == 1, "loop callback disabled");
    Expect(HasRecord("sandbox", "callback disabled after 3 faults", "mod=loop"), "disable logged");

    SetMod("pcallloop", "wum.timers.after(0, function() pcall(function() while true do end end) reached = true end)\n");
    Expect(sandbox::LoadMod("pcallloop"), "pcallloop loads");
    Frames(1);
    ExpectEq(Eval("pcallloop", "return reached"), "nil", "a mod's own pcall cannot swallow the budget");
    Expect(StatusOf("pcallloop").faults == 1, "pcallloop faulted");

    SetMod("pcallspin", "wum.timers.after(0, function() while true do pcall(error, 'x') end end)\n");
    Expect(sandbox::LoadMod("pcallspin"), "pcallspin loads");
    Frames(1);
    Expect(StatusOf("pcallspin").faults == 1, "while true do pcall() end is stopped");

    SetMod("coloop", "wum.timers.after(0, function() local co = coroutine.wrap(function() while true do end end) co() end)\n");
    Expect(sandbox::LoadMod("coloop"), "coloop loads");
    Frames(1);
    Expect(StatusOf("coloop").faults == 1, "coroutines share the budget");

    SetMod("xploop", "wum.timers.after(0, function() xpcall(function() while true do end end, function() while true do end end) done = true end)\n");
    Expect(sandbox::LoadMod("xploop"), "xploop loads");
    Frames(1);
    ExpectEq(Eval("xploop", "return done"), "nil", "xpcall with a looping handler is stopped");

    SetMod("closeloop", R"lua(
wum.timers.after(0, function()
  local co = coroutine.wrap(function()
    local x <close> = setmetatable({}, {__close = function() while true do end end})
    while true do end
  end)
  co()
end)
wum.timers.after(0, function()
  local co = coroutine.create(function()
    local x <close> = setmetatable({}, {__close = function() while true do end end})
    while true do end
  end)
  coroutine.resume(co)
  coroutine.close(co)
  after = true
end)
)lua");
    Expect(sandbox::LoadMod("closeloop"), "closeloop loads");
    Frames(1);
    Expect(StatusOf("closeloop").faults == 2, "__close handlers of a coroutine stopped by the budget cannot hang");
    ExpectEq(Eval(nullptr, "return xpcall(error, function(m) return 'handled:' .. m end, 'x')"), "false\thandled:x",
             "xpcall calls the handler");
    ExpectEq(Eval(nullptr, "return xpcall(function(a, b) return a + b end, print, 2, 3)"), "true\t5", "xpcall passes arguments");
    ExpectEq(Eval(nullptr, "local f = coroutine.wrap(function(a) local b = coroutine.yield(a + 1) return b * 2 end) return f(1), f(5)"),
             "2\t10", "coroutine.wrap");
    Expect(Eval(nullptr, "local f = coroutine.wrap(function() error('inner') end) f()").find("inner") != std::string::npos,
           "coroutine.wrap propagates errors");

    ExpectEq(Eval(nullptr, "return 1 + 1"), "2", "the VM is fine after runaway mods");
    for (const char* id : {"loop", "pcallloop", "pcallspin", "coloop", "xploop", "closeloop"}) sandbox::UnloadMod(id);
}

void TestMemory() {
    SetMod("alloc", "wum.timers.every(0, function() big = string.rep('x', 8 * 1024 * 1024) end)\n");
    Expect(sandbox::LoadMod("alloc"), "alloc loads");
    Frames(1);
    Expect(StatusOf("alloc").faults == 1, "ModMemoryMB is enforced");
    Expect(HasRecord("sandbox", "callback error", "out of memory") || HasRecord("sandbox", "callback error", "not enough memory"),
           "OOM logged");
    SetMod("allocp", "wum.timers.after(0, function() pcall(string.rep, 'x', 8 * 1024 * 1024) swallowed = true end)\n");
    Expect(sandbox::LoadMod("allocp"), "allocp loads");
    Frames(1);
    Expect(StatusOf("allocp").faults == 1, "a swallowed OOM is still a fault");
    SetMod("grow", "wum.timers.after(0, function() local t = {} for i = 1, 100 do t[i] = string.rep('x', 100000) .. i end end)\n");
    Expect(sandbox::LoadMod("grow"), "grow loads");
    Frames(1);
    Expect(StatusOf("grow").faults == 1, "growing a table past the limit faults");
    ExpectEq(Eval(nullptr, "return #string.rep('y', 1000)"), "1000", "the VM allocates after OOM faults");
    for (const char* id : {"alloc", "allocp", "grow"}) sandbox::UnloadMod(id);
    Expect(sandbox::SlotBytes(sandbox::FindMod("alloc")->slot) < (1u << 20), "unload releases memory");
}

void TestEvents() {
    SetMod("evA", R"lua(
got = {}
wum.events.on("mod.evB.ping", function(p, name) got[#got + 1] = name .. ":" .. p.n end)
wum.events.on("melange.test", function(p) posted = p.v end)
engineH = wum.events.on("GameLogic.Turn.Started", function(p, name) turns = (turns or 0) + 1 end)
)lua");
    SetMod("evB", R"lua(
wum.events.emit("ping", {n = 5})
ok, err = pcall(wum.events.emit, "mod.evA.fake", {})
)lua");
    Expect(sandbox::LoadMod("evA") && sandbox::LoadMod("evB"), "event mods load");
    ExpectEq(Eval("evA", "return #got"), "0", "emits are delivered at the next frame, not synchronously");
    Expect(fake::g_engineSubs.count("GameLogic.Turn.Started") == 1, "engine message subscribed on first listener");
    Expect(lua::PostEvent("melange.test", "{\"v\": 7}"), "PostEvent accepted");
    sandbox::QueueEvent("GameLogic.Turn.Started", [] {
        json::Value v;
        v.type = json::Type::Object;
        return v;
    }());
    Frames(1);
    ExpectEq(Eval("evA", "return got[1]"), "mod.evB.ping:5", "mod event delivered with payload and name");
    ExpectEq(Eval("evA", "return posted"), "7", "PostEvent delivered");
    ExpectEq(Eval("evA", "return turns"), "1", "engine event delivered");
    ExpectEq(Eval("evB", "return ok"), "false", "a mod cannot emit another mod's events");
    ExpectEq(Eval("evA", "return wum.events.off(engineH)"), "true", "off");
    Expect(fake::g_engineSubs.count("GameLogic.Turn.Started") == 0, "engine message unsubscribed on last listener");
    ExpectEq(Eval("evB", "return wum.events.off(1)"), "false", "off of a handle the mod does not own");
    sandbox::UnloadMod("evA");
    sandbox::UnloadMod("evB");
}

void TestTimers() {
    SetMod("tm", R"lua(
a, e = 0, 0
wum.timers.after(1, function() a = a + 1 end)
h = wum.timers.every(0.5, function() e = e + 1 end)
)lua");
    Expect(sandbox::LoadMod("tm"), "tm loads");
    Frames(1, 0.4);
    ExpectEq(Eval("tm", "return a, e"), "0\t0", "timers not yet due");
    Frames(1, 0.2);
    ExpectEq(Eval("tm", "return a, e"), "0\t1", "every fires");
    Frames(1, 0.5);
    ExpectEq(Eval("tm", "return a, e"), "1\t2", "after fires once");
    Frames(1, 0.5);
    ExpectEq(Eval("tm", "return a"), "1", "after does not repeat");
    Eval("tm", "wum.timers.cancel(h)");
    Frames(2, 1.0);
    ExpectEq(Eval("tm", "return e"), "3", "cancel stops every");
    Expect(StatusOf("tm").callbacks == 0, "no timers left");
    sandbox::UnloadMod("tm");
}

void TestReload() {
    const std::string v1 = R"lua(
counter = 0
wum.timers.every(0, function() counter = counter + 1 end)
wum.ui.panel("main", "Main", function() end)
wum.draw.on("hud", function() end)
tex = wum.draw.texture("icon.png")
wum.storage.set("k", {a = 1})
wum.mod.keep.count = 7
wum.mod.onReload = function(prev) prevCount = prev and prev.count; reloaded = true end
)lua";
    SetMod("rl", v1);
    Expect(sandbox::LoadMod("rl"), "rl v1 loads");
    const uint32_t cbs = StatusOf("rl").callbacks;
    Expect(cbs == 3, "rl has 3 handles: " + std::to_string(cbs));
    Expect(fake::g_panels.size() == 1 && fake::g_drawCbs == 1 && fake::g_textures == 1, "panel, draw callback and texture attached");
    Frames(2);
    ExpectEq(Eval("rl", "return counter"), "2", "v1 timer runs");

    SetMod("rl", "counter = = 1\n");
    Expect(!sandbox::ReloadMod("rl"), "syntax error reload fails");
    const std::string err = StatusOf("rl").error;
    Expect(err.find("rl/client/init.lua:1:") != std::string::npos, "error has the line number: " + err);
    Frames(1);
    ExpectEq(Eval("rl", "return counter"), "3", "the old version keeps running");
    Expect(fake::g_panels.size() == 1, "the old panel stays");

    SetMod("rl", "error('boom at top level')\n");
    Expect(!sandbox::ReloadMod("rl"), "runtime error reload fails");
    Frames(1);
    ExpectEq(Eval("rl", "return counter"), "4", "the old version survives a runtime error too");

    SetMod("rl", v1 + "version2 = true\n");
    Expect(sandbox::ReloadMod("rl"), "v2 reloads");
    ExpectEq(Eval("rl", "return reloaded, prevCount, version2, counter"), "true\t7\ttrue\t0", "onReload gets the previous keep");
    Expect(StatusOf("rl").callbacks == cbs, "handle count unchanged after reload");
    Expect(fake::g_panels.size() == 1 && fake::g_drawCbs == 1 && fake::g_textures == 1, "old handles revoked, no duplicates");
    ExpectEq(Eval("rl", "return wum.storage.get('k').a"), "1", "storage survives reload");
    Expect(HasRecord("sandbox", "reloaded", "mod=rl"), "reload logged");
    sandbox::UnloadMod("rl");
    Expect(fake::g_panels.empty() && fake::g_drawCbs == 0 && fake::g_textures == 0, "unload revokes everything");
}

void TestConfigStorage() {
    SetMod("cfg", "", R"(,"settings":[{"key":"show","type":"bool","default":true,"label":"Show"},
        {"key":"size","type":"int","default":3,"min":1,"max":10,"label":"Size"},
        {"key":"mode","type":"enum","options":["a","b"],"default":"a","label":"Mode"},
        {"key":"scale","type":"float","default":0.5,"label":"Scale"}])");
    Expect(sandbox::LoadMod("cfg"), "cfg loads");
    ExpectEq(Eval("cfg", "return wum.config.get('show'), wum.config.get('size'), wum.config.get('mode'), wum.config.get('scale')"),
             "true\t3\ta\t0.5", "config defaults");
    Expect(Eval("cfg", "return wum.config.set('size', 11)").rfind("ERR:", 0) == 0, "out-of-range set fails");
    Expect(Eval("cfg", "return wum.config.set('mode', 'c')").rfind("ERR:", 0) == 0, "bad enum fails");
    Expect(Eval("cfg", "return wum.config.get('nope')").rfind("ERR:", 0) == 0, "undeclared key fails");
    ExpectEq(Eval("cfg", "return wum.config.set('size', 5)"), "true", "set");
    ExpectEq(fake::g_config["Mod.cfg/size"], "5", "stored in [Mod.cfg]");
    ExpectEq(Eval("cfg", "return wum.config.get('size')"), "5", "get after set");

    ExpectEq(Eval("cfg", "wum.storage.set('b', {x = {1, 2, 3}, s = 'hi'}) wum.storage.set('a', 1.5) return table.concat(wum.storage.keys(), ',')"),
             "a,b", "storage keys");
    ExpectEq(Eval("cfg", "local t = wum.storage.get('b') return t.x[3], t.s, wum.storage.get('a')"), "3\thi\t1.5", "storage values");
    Expect(Eval("cfg", "wum.storage.set('f', print)").rfind("ERR:", 0) == 0, "functions are not storable");
    Expect(Eval("cfg", "wum.storage.set('u', '\\xff')").rfind("ERR:", 0) == 0, "invalid UTF-8 is not storable");
    Expect(Eval("cfg", "wum.storage.set('big', string.rep('x', 1100000))").rfind("ERR:", 0) == 0, "1 MB cap");
    Eval("cfg", "wum.storage.remove('a')");
    sandbox::UnloadMod("cfg");
    const std::string file = ReadText(fake::g_dataDir + L"\\mods\\cfg\\storage.json");
    Expect(file.find("\"b\":{\"s\":\"hi\",\"x\":[1,2,3]}") != std::string::npos && file.find("\"a\"") == std::string::npos,
           "storage written on unload: " + file);
    Expect(sandbox::LoadMod("cfg"), "cfg reloads");
    ExpectEq(Eval("cfg", "return wum.storage.get('b').s"), "hi", "storage read back from disk");
    sandbox::UnloadMod("cfg");
}

void TestConsole() {
    ExpectEq(Eval(nullptr, "return 1+1"), "2", "return expression");
    ExpectEq(Eval(nullptr, "1+2"), "3", "bare expression");
    ExpectEq(Eval(nullptr, "=6*7"), "42", "= shorthand");
    ExpectEq(Eval(nullptr, "for i=1,3 do print(i) end"), "1\n2\n3", "print is captured");
    ExpectEq(Eval(nullptr, "return {a = 1}"), "{a = 1}", "tables one level deep");
    Expect(Eval(nullptr, "x = ").rfind("ERR:", 0) == 0, "syntax error");
    Expect(Eval(nullptr, "wum.timers.after(1, print)").find("only mod code") != std::string::npos, "console cannot register callbacks");
    Expect(Eval(nullptr, "while true do end").find("instruction budget exceeded (100000 instructions per call)") != std::string::npos, "console budget");
    std::vector<std::string> out;
    sandbox::Complete(nullptr, "wum.ti", &out);
    Expect(out.size() == 1 && out[0] == "wum.timers", "complete wum.ti");
    sandbox::Complete(nullptr, "wum.dr", &out);
    Expect(out.size() == 1 && out[0] == "wum.draw", "complete wum.dr");
    sandbox::Complete(nullptr, "x = str", &out);
    Expect(!out.empty() && out[0] == "x = string", "complete globals after text");
    sandbox::Complete("esc", "sec", &out);
    Expect(out.size() == 1 && out[0] == "secret", "complete in a mod environment");
    sandbox::Complete(nullptr, "string.fo", &out);
    Expect(out.size() == 1 && out[0] == "string.format", "complete through a frozen table");
}

void TestUnsafe() {
    SetMod("dd", R"lua(
local p, u = wum.probe, wum.unsafe
r1 = u.read(p.buf, "u32")
u.write(p.buf + 4, "f32", 1.5)
r2 = u.read(p.buf + 4, "f32")
r3 = table.concat(u.read(p.buf, "u8", 4), ",")
okNull, errNull = pcall(u.read, 0, "u32")
okW, errW = pcall(u.write, 0, "u32", 1)
sum = u.call(p.add, "cdecl", 2, 40)
std = u.call(p.addStd, "stdcall", 4, 2)
half = u.call(p.half, "cdecl:f32", 3.0)
plus = u.call(p.plus, "thiscall", p.obj, 5)
okBad, errBad = pcall(u.call, 16, "cdecl")
s = u.read(p.str, "cstr")
base = u.base()
who = wum.who.who()
)lua", "", true, true);
    Expect(sandbox::LoadMod("dd"), "dd loads: " + StatusOf("dd").error);
    ExpectEq(Eval("dd", "return string.format('%x', r1), r2, r3"), "12345678\t1.5\t120,86,52,18", "read/write");
    ExpectEq(Eval("dd", "return okNull, okW"), "false\tfalse", "read and write of address 0 raise instead of crashing");
    Expect(Eval("dd", "return errNull").find("access violation") != std::string::npos, "fault message");
    ExpectEq(Eval("dd", "return sum, std, half, plus"), "42\t42\t1.5\t105", "call cdecl/stdcall/f32/thiscall");
    ExpectEq(Eval("dd", "return okBad"), "false", "a call to a bad address raises");
    ExpectEq(Eval("dd", "return s, base > 0"), "melange\ttrue", "cstr and base");
    ExpectEq(Eval("dd", "return who"), "dd", "lua::CurrentMod inside a library");
    Expect(HasRecord("thumper", "wum.unsafe fault", "mod=dd"), "faults logged in the thumper category");

    SetMod("ddno", "ok, err = pcall(wum.unsafe.read, 0, 'u32') hasProbe = wum.probe ~= nil\n", "", true, false);
    Expect(sandbox::LoadMod("ddno"), "ddno loads");
    ExpectEq(Eval("ddno", "return ok, err:match('Deep Desert not granted') ~= nil, hasProbe"), "false\ttrue\tfalse",
             "not granted: wum.unsafe raises, Deep Desert libraries hidden");
    Expect(HasRecord("thumper", "wum.unsafe refused", "mod=ddno"), "refusal logged");
    SetMod("plain", "hasUnsafe = wum.unsafe ~= nil\n");
    Expect(sandbox::LoadMod("plain"), "plain loads");
    ExpectEq(Eval("plain", "return hasUnsafe, wum.who.who()"), "false\tplain", "no wum.unsafe without the permission");

    for (fake::Mod& m : fake::g_mods)
        if (m.id == "ddno") m.granted = true;
    Expect(sandbox::ReloadMod("ddno"), "reload after grant");
    ExpectEq(Eval("ddno", "return pcall(wum.unsafe.base)"), "true\t" + std::to_string(game::Base()), "granted after reload");
    for (const char* id : {"dd", "ddno", "plain"}) sandbox::UnloadMod(id);
}

void TestPanels() {
    SetMod("ui", "", R"(,"settings":[{"key":"show","type":"bool","default":false,"label":"Show"}])");
    WriteText(fake::g_modsDir + L"\\ui\\client\\init.lua", R"lua(
frames = 0
wum.ui.panel("p", "Panel", function()
  frames = frames + 1
  local v, changed = wum.ui.checkbox("Show", wum.config.get("show"))
  if changed then wum.config.set("show", v) end
  wum.ui.text("hello")
  if wum.ui.button("Go") then end
  if frames >= 2 then error("panel bug") end
end)
ok, err = pcall(wum.ui.text, "outside")
wum.draw.hudText(1, 2, "x", "#ff000080")
)lua");
    Expect(sandbox::LoadMod("ui"), "ui loads: " + StatusOf("ui").error);
    ExpectEq(Eval("ui", "return ok"), "false", "widgets outside a panel raise");
    Expect(draw::g_lastHudColor == 0x800000ffu, "colour #RRGGBBAA converted to 0xAABBGGRR");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.IniFilename = nullptr;
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    Expect(fake::g_panels.size() == 1 && fake::g_panels[0].id == "mod.ui.p", "panel registered with a namespaced id");
    for (int f = 0; f < 5; ++f) {
        ImGui::NewFrame();
        ImGui::Begin("host");
        for (auto& p : fake::g_panels) p.fn(p.user);
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
    ExpectEq(Eval("ui", "return frames"), "4", "panel ran until disabled after 3 faults");
    Expect(StatusOf("ui").disabledCallbacks == 1, "panel callback disabled");
    sandbox::UnloadMod("ui");
    Expect(fake::g_panels.empty(), "panel removed on unload");
}

void TestMenus() {
    SetMod("menu", R"lua(
on = false
wum.ui.menu("Grid", function() on = not on end, { checked = function() return on end })
wum.ui.menu("Plain", function() end)
wum.ui.menu("Broken", function() end, { checked = function() error("state bug") end })
ok = pcall(wum.ui.menu, "Bad", function() end, { checked = true })
)lua");
    Expect(sandbox::LoadMod("menu"), "menu loads: " + StatusOf("menu").error);
    auto find = [](const char* path) -> const fake::MenuReg* {
        for (const fake::MenuReg& r : fake::g_menus)
            if (r.path == path) return &r;
        return nullptr;
    };
    const fake::MenuReg* grid = find("Mods/menu/Grid");
    const fake::MenuReg* plain = find("Mods/menu/Plain");
    const fake::MenuReg* broken = find("Mods/menu/Broken");
    Expect(grid && plain && broken && grid->checked && plain->checked, "menu items registered with a state getter");
    if (!grid || !plain || !broken || !grid->checked) return;
    Expect(!grid->checked(grid->user), "unchecked until the mod says so");
    grid->fn(grid->user);
    Expect(grid->checked(grid->user), "checked after the item flips the mod's state");
    Expect(!plain->checked(plain->user), "an item without opts.checked never shows a check mark");
    bool any = false;
    for (int i = 0; i < 5; ++i) any = broken->checked(broken->user) || any;
    Expect(!any, "a failing getter reads unchecked");
    Expect(StatusOf("menu").disabledCallbacks == 1, "a failing getter is disabled after 3 faults");
    ExpectEq(Eval("menu", "return ok"), "false", "opts.checked must be a function");
    grid->fn(grid->user);
    ExpectEq(Eval("menu", "return on"), "false", "the action still runs");
    sandbox::UnloadMod("menu");
    Expect(!grid->checked(grid->user), "unchecked once the mod is unloaded");
}

void CopySample(const std::string& id, bool unsafe, bool granted) {
    const std::wstring src = W(MELANGE_SOURCE_DIR) + L"\\dist\\Mods\\" + W(id);
    SetMod(id, ReadText(src + L"\\client\\init.lua"), "", unsafe, granted);
    WriteText(fake::g_modsDir + L"\\" + W(id) + L"\\spice.json", ReadText(src + L"\\spice.json"));
}

void TestSamples() {
    CopySample("hello-spice", false, false);
    Expect(sandbox::LoadMod("hello-spice"), "hello-spice loads: " + StatusOf("hello-spice").error);
    const std::string manifest = ReadText(fake::g_modsDir + L"\\hello-spice\\spice.json");
    Expect(manifest.find("\"defaultEnabled\": false") != std::string::npos, "hello-spice ships disabled");
    sandbox::Game().inMatch = true;
    sandbox::QueueEvent("GameLogic.Turn.Started", [] {
        json::Value v;
        v.type = json::Type::Object;
        return v;
    }());
    Frames(60, 0.05);
    sandbox::Game().inMatch = false;
    Expect(StatusOf("hello-spice").faults == 0, "hello-spice runs without faults");
    Expect(HasRecord("mod", "GameLogic.Turn.Started\tturn\t1\t{}", "mod=hello-spice"), "hello-spice logs the turn");
    SetMod("hello-spice", ReadText(W(MELANGE_SOURCE_DIR) + L"\\dist\\Mods\\hello-spice\\client\\init.lua"));
    WriteText(fake::g_modsDir + L"\\hello-spice\\spice.json", manifest);
    Expect(sandbox::ReloadMod("hello-spice"), "hello-spice hot reload");
    Expect(HasRecord("mod", "hello-spice reloaded at turn\t1"), "hello-spice keeps its turn across reload");
    sandbox::UnloadMod("hello-spice");
    Expect(ReadText(fake::g_dataDir + L"\\mods\\hello-spice\\storage.json").find("\"frames\"") != std::string::npos,
           "hello-spice counts frames in wum.storage");

    CopySample("deep-desert-demo", true, false);
    Expect(sandbox::LoadMod("deep-desert-demo"), "deep-desert-demo loads without the grant");
    Expect(HasRecord("mod", "deep-desert-demo:", "mod=deep-desert-demo") && HasRecord("thumper", "wum.unsafe refused", "mod=deep-desert-demo"),
           "deep-desert-demo refused and says so");
    sandbox::UnloadMod("deep-desert-demo");
    for (fake::Mod& m : fake::g_mods)
        if (m.id == "deep-desert-demo") m.granted = true;
    Expect(sandbox::LoadMod("deep-desert-demo"), "deep-desert-demo loads with the grant");
    bool stamp = false;
    for (const auto& r : fake::g_recs) stamp |= r.cat == "mod" && r.msg.find("PE timestamp") != std::string::npos;
    Expect(stamp, "deep-desert-demo reads the PE header");
    sandbox::UnloadMod("deep-desert-demo");
}

void TestGame() {
    gamestate::g_readable = false;
    ExpectEq(Eval(nullptr, "return select('#', wum.game.worms()), select(2, wum.game.worms())"), "2\tunavailable",
             "worms unavailable without the readers");
    ExpectEq(Eval(nullptr, "return wum.game.activeWorm()"), "nil", "no active worm without the readers");
    ExpectEq(Eval(nullptr, "return wum.game.theme()"), "nil", "no theme without the readers");
    gamestate::Snapshot& s = gamestate::g_snapshot;
    s = {};
    s.match.inMatch = true;
    s.match.activeWorm = 5;
    s.teamCount = 1;
    s.teams[0] = {};
    s.teams[0].slot = 1;
    s.teams[0].active = s.teams[0].ai = true;
    strcpy(s.teams[0].name, "Sandworms");
    s.wormCount = 2;
    s.worms[0] = {};
    s.worms[0].slot = 5;
    s.worms[0].team = 1;
    s.worms[0].health = 87;
    s.worms[0].alive = s.worms[0].active = true;
    s.worms[0].weapon = 1;
    s.worms[0].pos = {1.5f, -2.f, 300.f};
    s.worms[0].vel = {0.25f, -0.5f, 0.f};
    s.worms[0].yaw = 4.25f;
    strcpy(s.worms[0].name, "Paul");
    s.worms[1] = {};
    s.worms[1].slot = 6;
    s.worms[1].weapon = -1;
    strcpy(s.worms[1].name, "Leto");
    gamestate::g_readable = true;
    ExpectEq(Eval(nullptr, "local w = wum.game.worms() return #w, w[1].slot, w[1].team, w[1].name, w[1].health, "
                           "w[1].alive, w[1].weapon, w[1].pos.x, w[1].pos.y, w[1].pos.z"),
             "2\t5\t1\tPaul\t87\ttrue\t1\t1.5\t-2\t300", "worms fields");
    ExpectEq(Eval(nullptr, "return wum.game.worms()[1].yaw"), "4.25", "worm yaw");
    ExpectEq(Eval(nullptr, "local v = wum.game.worms()[1].vel return v.x, v.y, v.z"), "250\t-500\t0",
             "worm velocity in units per second (the engine's per-millisecond value x1000)");
    ExpectEq(Eval(nullptr, "local w = wum.game.worms()[2] return w.name, w.alive, w.weapon"), "Leto\tfalse\tnil",
             "a dead worm without a weapon");
    ExpectEq(Eval(nullptr, "local t = wum.game.teams() return #t, t[1].slot, t[1].name, t[1].active, t[1].ai, t[1][\"local\"]"),
             "1\t1\tSandworms\ttrue\ttrue\tfalse", "teams fields");
    ExpectEq(Eval(nullptr, "return wum.game.activeWorm()"), "5", "active worm slot");
    ExpectEq(Eval(nullptr, "return wum.game.theme()"), "nil", "no theme before the level reports one");
    strcpy(s.match.theme, "SPACE");
    ExpectEq(Eval(nullptr, "return wum.game.theme()"), "SPACE", "level theme");
    s.match.inMatch = false;
    ExpectEq(Eval(nullptr, "return wum.game.theme()"), "nil", "no theme outside a match");
    s.match.inMatch = true;
    s.match.activeWorm = -1;
    ExpectEq(Eval(nullptr, "return wum.game.activeWorm()"), "nil", "no active worm between turns");
    gamestate::g_readable = false;

    // wum.game.landRay: arguments are checked before the engine is asked; each result maps to its Lua form.
    gamestate::g_rayCalls = 0;
    Expect(Eval(nullptr, "return wum.game.landRay(1, 2, 3, 4, 5)").rfind("ERR:", 0) == 0, "landRay needs six numbers");
    Expect(Eval(nullptr, "return wum.game.landRay(1, 2, 3, 4, 5, 'x')").rfind("ERR:", 0) == 0,
           "landRay refuses a non-number");
    Expect(gamestate::g_rayCalls == 0, "bad arguments never reach the engine");
    gamestate::g_rayResult = gamestate::LandRayResult::Unavailable;
    ExpectEq(Eval(nullptr, "local r = table.pack(wum.game.landRay(0, 0, 0, 1, 1, 1)) return r.n, r[1], r[2]"),
             "2\tnil\tunavailable", "landRay unavailable");
    gamestate::g_rayResult = gamestate::LandRayResult::Budget;
    ExpectEq(Eval(nullptr, "return wum.game.landRay(0, 0, 0, 1, 1, 1)"), "nil\tbudget", "landRay over budget");
    gamestate::g_rayResult = gamestate::LandRayResult::Invalid;
    ExpectEq(Eval(nullptr, "return wum.game.landRay(0/0, 0, 0, 1, 1, 1)"), "nil\tinvalid", "landRay invalid point");
    gamestate::g_rayResult = gamestate::LandRayResult::Miss;
    ExpectEq(Eval(nullptr, "return select('#', wum.game.landRay(1.5, 2, 3, 4, 5, 6.25))"), "1", "a miss is one nil");
    Expect(gamestate::g_rayA.x == 1.5f && gamestate::g_rayA.z == 3.f && gamestate::g_rayB.y == 5.f &&
               gamestate::g_rayB.z == 6.25f,
           "landRay passes the segment through");
    gamestate::g_rayResult = gamestate::LandRayResult::Hit;
    gamestate::g_rayHit = {0.5f, {0.f, 1.f, 0.f}};
    ExpectEq(Eval(nullptr, "return wum.game.landRay(0, 10, 0, 0, -10, 0)"), "0.5\t0\t1\t0", "landRay hit: t and normal");
    gamestate::g_rayResult = gamestate::LandRayResult::Unavailable;
}

void TestGraphics() {
    ExpectEq(Eval("esc", "return wum.graphics.setShadowMapSize(4096)"), "true", "shadow-map request accepted");
    Expect(graphics::g_lastMod == "esc" && graphics::g_lastSize == 4096, "request carries the calling mod");
    Eval("esc", "wum.graphics.setShadowMapSize()");
    Expect(graphics::g_lastSize == -1, "nil clears the runtime request");
    Expect(Eval("esc", "return wum.graphics.setShadowMapSize(3000)").rfind("ERR:", 0) == 0, "an odd size is refused");
    ExpectEq(Eval("esc", "local s = wum.graphics.shadowMap() return s.size, s.effective, s.vanilla, s.modRequest"),
             "2048\t2048\t1024\ttrue", "shadowMap() fields");
    ExpectEq(Eval("esc", "return wum.graphics.setSupersample(4)"), "true", "supersample request accepted");
    Expect(graphics::g_lastMod == "esc" && graphics::g_lastSamples == 4, "supersample request carries the calling mod");
    Eval("esc", "wum.graphics.setSupersample()");
    Expect(graphics::g_lastSamples == 0, "nil clears the supersample request");
    Expect(Eval("esc", "return wum.graphics.setSupersample(16)").rfind("ERR:", 0) == 0, "16 samples is refused to a mod");
    ExpectEq(Eval("esc", "local s = wum.graphics.supersample() return s.x, s.y, s.effective, s.sceneWidth, s.multisampled"),
             "2\t2\t4\t3840\tfalse", "supersample() fields");
    ExpectEq(Eval("esc", "wum.shaders.setParam('Landscape.cg', '*FragmentMain', 'softness', 1.5) return 1"), "1",
             "a declared shader param is set");
    ExpectEq(Eval("esc", "return wum.postfx.setTransient('esc/glow', 'amount', 1, 2)"), "true", "setTransient on the mod's own effect");
    Expect(Eval("other", "wum.postfx.setTransient('esc/glow', 'amount', 1)").rfind("ERR:", 0) == 0, "setTransient refuses another mod's effect");
    Expect(Eval("other", "wum.shaders.setParam('Landscape.cg', '*FragmentMain', 'softness', 1)").rfind("ERR:", 0) == 0,
           "another mod's shader param is refused");
    ExpectEq(Eval("esc", "wum.shaders.enableGlsl('Landscape.cg', 'LandscapeFragmentMain', false) return 1"), "1",
             "an own GLSL replacement is paused");
    Expect(shaders::g_glslEntry == "LandscapeFragmentMain" && !shaders::g_glslOn, "pause carries the entry and state");
    Expect(Eval("other", "wum.shaders.enableGlsl('Landscape.cg', 'LandscapeFragmentMain', true)").rfind("ERR:", 0) == 0,
           "another mod's GLSL replacement is refused");
}

void TestSprites() {
    SetMod("spr", "tex = wum.draw.texture('spark.png') tex2 = wum.draw.texture('smoke.png')\n");
    SetMod("spr2", "tex = wum.draw.texture('other.png')\n");
    Expect(sandbox::LoadMod("spr") && sandbox::LoadMod("spr2"), "sprite mods load");
    ExpectEq(Eval("spr", "return type(wum.draw.sprite)"), "function", "wum.draw.sprite exists");

    draw::g_stage = render::Stage::Count;
    const std::string outside = Eval("spr", "return wum.draw.sprite(tex, 0, 0, 0, 1)");
    Expect(outside.find("only inside a wum.draw.on") != std::string::npos, "outside a draw callback raises: " + outside);
    draw::g_stage = render::Stage::Hud;
    Expect(Eval("spr", "return wum.draw.sprite(tex, 0, 0, 0, 1)").rfind("ERR:", 0) == 0, "a hud callback raises");

    draw::g_stage = render::Stage::World;
    draw::g_sprites.clear();
    ++draw::g_frame;
    ExpectEq(Eval("spr", "return wum.draw.sprite(tex, 1, 2, 3, 4, 5, 0, 0, 2, 0xff000080, 'additive')"), "true", "full call");
    Expect(draw::g_sprites.size() == 1, "one sprite submitted");
    if (!draw::g_sprites.empty()) {
        const draw::Sprite& s = draw::g_sprites[0];
        Expect(s.pos[0] == 1 && s.pos[1] == 2 && s.pos[2] == 3 && s.halfW == 4 && s.halfL == 5 && s.axis[0] == 0 &&
                   s.axis[1] == 0 && s.axis[2] == 2,
               "position, sizes and axis passed through");
        Expect(s.color == 0x800000ffu && s.blend == draw::SpriteBlend::Additive, "colour 0xRRGGBBAA -> 0xAABBGGRR, additive");
        ExpectEq(Eval("spr", "return tex"), std::to_string(s.texture), "texture id passed through");
    }
    ExpectEq(Eval("spr", "return wum.draw.sprite(tex2, 0, 0, 0, 1)"), "true", "only tex, position and halfW are required");
    Expect(draw::g_sprites.back().color == 0xffffffffu && draw::g_sprites.back().blend == draw::SpriteBlend::Alpha &&
               draw::g_sprites.back().halfL == 0 && draw::g_sprites.back().axis[2] == 0,
           "defaults: white, alpha, billboard");
    Eval("spr", "wum.draw.sprite(tex, 0, 0, 0, 1, 1, 1, 0, 0, '#11223344', 'premul')");
    Expect(draw::g_sprites.back().color == 0x44332211u && draw::g_sprites.back().blend == draw::SpriteBlend::Premultiplied,
           "\"#RRGGBBAA\" colour, premul mode");
    Eval("spr", "wum.draw.sprite(tex, 0, 0, 0, 1, 1, 1, 0, 0, {1, 0, 0, 0.5}, 'alpha')");
    Expect(draw::g_sprites.back().color == 0x800000ffu && draw::g_sprites.back().blend == draw::SpriteBlend::Alpha,
           "{r, g, b, a} colour, alpha mode");
    draw::g_stage = render::Stage::WorldLate;
    ExpectEq(Eval("spr", "return wum.draw.sprite(tex, 0, 0, 0, 1)"), "true", "worldLate callbacks may draw sprites");
    ExpectEq(Eval("spr", "return wum.draw.sprite(tex, 0/0, 1/0, 0, 1, 0/0, 0, 0, 0)"), "true",
             "non-finite values are not an error (the draw layer skips them)");

    const size_t before = draw::g_sprites.size();
    const char* bad[] = {
        "wum.draw.sprite('x', 0, 0, 0, 1)",                  // texture not a number
        "wum.draw.sprite(1.5, 0, 0, 0, 1)",                  // texture not an integer
        "wum.draw.sprite(9999, 0, 0, 0, 1)",                 // never loaded
        "wum.draw.sprite(0, 0, 0, 0, 1)",                    // zero
        "wum.draw.sprite(tex, 'a', 0, 0, 1)",                // position
        "wum.draw.sprite(tex, 0, 0, nil, 1)",                // missing z
        "wum.draw.sprite(tex, 0, 0, 0)",                     // missing halfW
        "wum.draw.sprite(tex, 0, 0, 0, 1, 'long')",          // halfL
        "wum.draw.sprite(tex, 0, 0, 0, 1, 1, {}, 0, 0)",     // axis component
        "wum.draw.sprite(tex, 0, 0, 0, 1, 1, 0, 0, 0, true)",  // colour
        "wum.draw.sprite(tex, 0, 0, 0, 1, 1, 0, 0, 0, '#12')",  // colour string
        "wum.draw.sprite(tex, 0, 0, 0, 1, 1, 0, 0, 0, nil, 'add')",  // mode
        "wum.draw.sprite(tex, 0, 0, 0, 1, 1, 0, 0, 0, nil, {})",     // mode type
    };
    for (const char* code : bad) Expect(Eval("spr", code).rfind("ERR:", 0) == 0, std::string("raises: ") + code);
    const std::string foreign = Eval("spr2", "return wum.draw.sprite(" + Eval("spr", "return tex") + ", 0, 0, 0, 1)");
    Expect(foreign.find("texture id from this mod's wum.draw.texture expected") != std::string::npos,
           "another mod's texture is refused: " + foreign);
    Expect(draw::g_sprites.size() == before, "nothing submitted by a failed call");

    // Per-mod, per-frame cap (4096), counted across callbacks and reset by the next frame.
    ++draw::g_frame;
    const std::string loop = "local n = 0 for i = 1, 2100 do if wum.draw.sprite(tex, i, 0, 0, 1) then n = n + 1 end end return n";
    ExpectEq(Eval("spr", loop), "2100", "under the cap");
    ExpectEq(Eval("spr", loop), "1996", "the cap stops the 4097th sprite of the frame");
    ExpectEq(Eval("spr", "return wum.draw.sprite(tex, 0, 0, 0, 1)"), "false", "past the cap returns false");
    ExpectEq(Eval("spr2", "return wum.draw.sprite(tex, 0, 0, 0, 1)"), "true", "the cap is per mod");
    ++draw::g_frame;
    ExpectEq(Eval("spr", "return wum.draw.sprite(tex, 0, 0, 0, 1)"), "true", "the next frame has a fresh budget");
    draw::g_spriteCap = draw::g_sprites.size();
    ExpectEq(Eval("spr2", "return wum.draw.sprite(tex, 0, 0, 0, 1)"), "false", "the draw layer's stage cap returns false");
    draw::g_spriteCap = draw::kMaxSpritesPerStage;

    // Lua-side cost: 2000 calls per Eval (the selftest's 100k instruction budget), timed over several frames.
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    const std::string bench =
        "local s, sp, t = wum.draw.sprite, 0, tex for i = 1, 2000 do s(t, i, 2, 3, 4, 8, 0.5, 0.25, 1, 0xffcc88ff, 'additive') end";
    double best = 1e9;
    for (int rep = 0; rep < 8; ++rep) {
        ++draw::g_frame;
        draw::g_sprites.clear();
        QueryPerformanceCounter(&t0);
        const std::string r = Eval("spr", bench);
        QueryPerformanceCounter(&t1);
        Expect(r.rfind("ERR:", 0) != 0 && draw::g_sprites.size() == 2000, "bench loop ran: " + r);
        best = std::min(best, static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
    }
    printf("  sprite: Lua wum.draw.sprite %.3f ms per 1000 calls (best of 8, incl. Eval overhead)\n", best / 2.0);

    draw::g_stage = render::Stage::Count;
    draw::g_sprites.clear();
    sandbox::UnloadMod("spr");
    sandbox::UnloadMod("spr2");
}

void TestDocs() {
    const std::string doc = ReadText(W(MELANGE_SOURCE_DIR) + L"\\docs\\lua-api.md");
    Expect(!doc.empty(), "docs/lua-api.md exists");
    int missing = 0;
    for (const std::string& n : sandbox::ApiNames()) {
        if (n.rfind("wum.probe.", 0) == 0 || n.rfind("wum.who.", 0) == 0) continue;
        if (doc.find("`" + n) == std::string::npos) {
            printf("  undocumented: %s\n", n.c_str());
            ++missing;
        }
    }
    Expect(missing == 0, "every wum.* name is in docs/lua-api.md");
    Expect(sandbox::ApiNames().size() > 60, "API listing is complete");
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fake::g_root = std::wstring(tmp) + L"melange_sandbox_selftest_" + std::to_wstring(GetCurrentProcessId());
    fake::g_modsDir = fake::g_root + L"\\Mods";
    fake::g_dataDir = fake::g_root + L"\\data";
    CreateDirectoryW(fake::g_root.c_str(), nullptr);
    CreateDirectoryW(fake::g_modsDir.c_str(), nullptr);
    CreateDirectoryW(fake::g_dataDir.c_str(), nullptr);

    Expect(lua::AddLibrary("probe", &OpenProbe, lua::kLibDeepDesert), "AddLibrary (Deep Desert)");
    Expect(lua::AddLibrary("who", &OpenWho), "AddLibrary");
    Expect(!lua::AddLibrary("log", &OpenWho), "AddLibrary refuses a built-in name");
    Expect(!lua::AddLibrary("who", &OpenWho), "AddLibrary refuses a duplicate");

    sandbox::Limits lim;
    lim.instrPerCall = 100000;
    lim.modBytes = 4u << 20;
    lim.totalBytes = 64u << 20;
    Expect(!lua::Ready(), "not ready before Start");
    Expect(sandbox::Start(lim), "VM starts");
    Expect(lua::Ready() && lua::ClientState() != nullptr, "ready");

    printf("  started\n");
    const std::pair<const char*, void (*)()> tests[] = {
        {"escapes", TestEscapes}, {"budget", TestBudget},   {"memory", TestMemory},
        {"events", TestEvents},   {"timers", TestTimers},   {"reload", TestReload},
        {"config/storage", TestConfigStorage},              {"console", TestConsole},
        {"unsafe", TestUnsafe},   {"panels", TestPanels},   {"menus", TestMenus},
        {"samples", TestSamples},
        {"game", TestGame},       {"graphics", TestGraphics}, {"sprites", TestSprites},
        {"docs", TestDocs}};
    for (const auto& [name, fn] : tests) {
        const int before = g_fail;
        fn();
        printf("  %-15s %s\n", name, g_fail == before ? "ok" : "FAILED");
    }

    const lua::Stats s = lua::GetStats();
    Expect(s.faults >= 10 && s.instructions > 0 && s.bytes > 0, "stats");
    sandbox::Stop();
    Expect(!lua::Ready() && sandbox::TotalBytes() == 0, "Stop frees the VM");

    std::error_code ec;
    std::filesystem::remove_all(fake::g_root, ec);
    printf("sandbox_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
