// Offline self-test for the Sandbox (client Lua VM): environments, escapes, limits, faults, hot reload, events, timers,
// config, storage, console eval, wum.unsafe, panels and the API reference. Thumper, the overlay, draw, render, postfx,
// config, jlog and the game are replaced by fakes below. Exit code 0 = all passed.
#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "audio/clips.h"
#include "core/config.h"
#include "core/game.h"
#include "core/log.h"
#include "input/controls.h"
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
#include "media/media.h"
#include "media/pack.h"
#include "net/peerbus.h"

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
bool g_ctlAvail = true;
melange::controls::Options g_ctlOpts;
const void* g_ctlOwner = nullptr;
struct AudioVoice {
    const void* owner;
    uint32_t clip;
    melange::audio::PlayOpts opts;
};
bool g_audioAvail = true;
int g_audioReleases = 0;
uint32_t g_audioNext = 0;
std::map<const void*, melange::audio::ClipCache> g_audioClips;  // the real cache; only the XAudio2 side is faked
std::map<uint32_t, AudioVoice> g_audioVoices;
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
    if (text && !strcmp(text, "F5")) { *dik = 0x3F; *mods = 0; }  // a bare function key
    if (text && !strcmp(text, "Q")) { *dik = 0x10; *mods = 0; }   // a bare ordinary key
    return text && *text;
}
}  // namespace overlay
namespace controls {  // the Controls module is replaced by a fake that remembers what wum.input set
bool Available() { return fake::g_ctlAvail; }
void SetOptions(const Options& o, const void* owner) {
    fake::g_ctlOpts = o;
    fake::g_ctlOwner = owner;
}
void ClearAllOptions() { SetOptions(Options{}, nullptr); }
void ClearOptions(const void* owner) {
    if (owner && fake::g_ctlOwner == owner) ClearAllOptions();
}
Options Effective() { return fake::g_ctlOpts; }
bool SmoothMouse() { return true; }
bool ActiveGroups(std::vector<std::string>* out) {
    if (!fake::g_ctlAvail) return false;
    *out = {"InGame", "WormFirstPersonAiming"};
    return true;
}
bool Binding(const char* name, std::string* label) {
    if (strcmp(name, "Worm.Jump") != 0) return false;
    *label = "Space";
    return true;
}
}  // namespace controls
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
TextureOptions g_lastOpts;
std::vector<unsigned> g_arrays;
unsigned LoadTextureEx(const wchar_t* path, const TextureOptions& o, const char** why) {
    if (wcsstr(path, L"odd.png")) {
        *why = "image height is not a multiple of layers";
        return 0;
    }
    g_lastOpts = o;
    ++fake::g_textures;
    if (o.layers > 0) g_arrays.push_back(g_nextTexture);
    return g_nextTexture++;
}
bool GetTextureInfo(unsigned tex, TextureInfo* out) {
    const bool array = std::find(g_arrays.begin(), g_arrays.end(), tex) != g_arrays.end();
    *out = {array ? 0x8C1Au : 0x0DE1u, 64, 64, array ? 4 : 0, false};
    return true;
}
void FreeTexture(unsigned) { --fake::g_textures; }
struct MeshCall {
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;
    float model[16];
    bool hasModel;
    unsigned texture;
    uint32_t flags;
};
std::vector<MeshCall> g_meshes;
void Mesh(const Vertex* v, uint32_t nv, const uint16_t* idx, uint32_t ni, const float* model, unsigned texture, uint32_t flags,
          int) {
    MeshCall c{};
    c.v.assign(v, v + nv);
    c.idx.assign(idx, idx + ni);
    c.hasModel = model != nullptr;
    if (model) memcpy(c.model, model, sizeof c.model);
    c.texture = texture;
    c.flags = flags;
    g_meshes.push_back(std::move(c));
}
int AddDrawCallback(render::Stage, DrawFn, void*, int) {
    ++fake::g_drawCbs;
    return fake::g_nextHandle++;
}
void RemoveDrawCallback(int) { --fake::g_drawCbs; }
}  // namespace draw
namespace render {
bool g_haveCam = false;
Camera g_cam{}, g_gameCam{};
bool GetCamera(Camera* c) {
    if (!g_haveCam) return false;
    *c = g_cam;
    return true;
}
bool GetGameCamera(Camera* c) {
    if (!g_haveCam) return false;
    *c = g_gameCam;
    return true;
}
const void* g_ovOwner = nullptr;
CameraOverride g_ov{};
int g_ovFrames = -1, g_ovSets = 0;
OverrideResult g_ovResult = OverrideResult::Ok;
OverrideResult SetCameraOverride(const void* owner, const CameraOverride& o, int frames) {
    if (g_ovResult != OverrideResult::Ok) return g_ovResult;
    if (g_ovOwner && g_ovOwner != owner) return OverrideResult::Busy;
    g_ovOwner = owner;
    g_ov = o;
    g_ovFrames = frames;
    ++g_ovSets;
    return OverrideResult::Ok;
}
bool ClearCameraOverride(const void* owner) {
    if (!g_ovOwner || (owner && owner != g_ovOwner)) return false;
    g_ovOwner = nullptr;
    return true;
}
bool CameraOverridden() { return g_ovOwner != nullptr; }
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
std::string g_texSampler;
unsigned g_texSet = ~0u;
std::vector<unsigned> g_texForgotten;
bool g_texInactive = false;
TextureResult SetOwnTexture(const char* owner, const char*, const char*, const char* sampler, unsigned tex) {
    if (strncmp(sampler, "mg_", 3) == 0) return TextureResult::BadName;
    if (g_texInactive) return TextureResult::Inactive;
    if (strcmp(owner, "shtex") != 0) return TextureResult::NotOwner;
    g_texSampler = sampler;
    g_texSet = tex;
    return TextureResult::Ok;
}
void ForgetTexture(unsigned tex) { g_texForgotten.push_back(tex); }
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
namespace audio {  // the Audio module is replaced by a fake that records plays; clips go through the real cache
bool Available() { return fake::g_audioAvail; }
uint32_t FindClip(const void* owner, const std::string& key) {
    auto it = fake::g_audioClips.find(owner);
    return it == fake::g_audioClips.end() ? 0 : it->second.Find(key);
}
uint32_t LoadClip(const void* owner, const std::string& key, const uint8_t* bytes, size_t n, std::string* why) {
    return fake::g_audioClips[owner].Add(key, bytes, n, why);
}
uint32_t Play(const void* owner, uint32_t clip, const PlayOpts& opts, std::string* why) {
    if (!fake::g_audioAvail) {
        *why = "audio unavailable";
        return 0;
    }
    auto it = fake::g_audioClips.find(owner);
    if (it == fake::g_audioClips.end() || !it->second.Get(clip)) {
        *why = "unknown sound";
        return 0;
    }
    fake::g_audioVoices[++fake::g_audioNext] = {owner, clip, opts};
    return fake::g_audioNext;
}
bool Stop(const void* owner, uint32_t voice) {
    auto it = fake::g_audioVoices.find(voice);
    if (it == fake::g_audioVoices.end() || it->second.owner != owner) return false;
    fake::g_audioVoices.erase(it);
    return true;
}
size_t StopAll(const void* owner) {
    size_t n = 0;
    for (auto it = fake::g_audioVoices.begin(); it != fake::g_audioVoices.end();) {
        if (it->second.owner == owner) {
            it = fake::g_audioVoices.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}
void Release(const void* owner) {
    ++fake::g_audioReleases;
    StopAll(owner);
    fake::g_audioClips.erase(owner);
}
}  // namespace audio

// Melange 0.12 (Holo): one fakes file per namespace owner.
#include "sandbox_fakes_media.inc"
#include "sandbox_fakes_peer.inc"
#include "sandbox_fakes_input.inc"
#include "sandbox_fakes_draw.inc"
#include "sandbox_fakes_audio.inc"
#include "sandbox_fakes_game.inc"
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

void TestMatch() {
    gamestate::Snapshot& s = gamestate::g_snapshot;
    gamestate::g_readable = false;
    ExpectEq(Eval(nullptr, "return wum.game.match()"), "nil", "match() is nil without the readers");
    s = {};
    gamestate::g_readable = true;
    ExpectEq(Eval(nullptr, "return wum.game.match()"), "nil", "match() is nil outside a match");
    s.match.inMatch = true;
    s.match.roundMs = 300000;
    s.match.roundMsLeft = 123456;
    s.match.turnMs = 45000;
    s.match.turnMsLeft = 9000;
    s.match.waterLevel = -12.5f;
    s.match.suddenDeath = true;
    s.match.turnsStarted = 7;
    ExpectEq(Eval(nullptr, "local m = wum.game.match() return m.roundMs, m.roundMsLeft, m.turnMs, m.turnMsLeft, m.waterLevel, "
                           "m.suddenDeath, m.turnsStarted, m.landMin"),
             "300000\t123456\t45000\t9000\t-12.5\ttrue\t7\tnil", "match() fields; no land bounds before the level reports them");
    s.match.haveLand = true;
    s.match.landMin = {-900.f, -40.f, -250.f};
    s.match.landMax = {1100.f, 610.f, 250.f};
    ExpectEq(Eval(nullptr, "local m = wum.game.match() return m.landMin.x, m.landMin.y, m.landMax.y, m.landMax.z"),
             "-900\t-40\t610\t250", "match() land bounds");
    ExpectEq(Eval(nullptr, "return math.type(wum.game.match().roundMs)"), "integer", "times are integers");
    s = {};
    gamestate::g_readable = false;
}

// A one-triangle mesh in the "pnt" layout, facing +Y.
const char* kTri = "verts = {0,0,0, 0,1,0, 0,0,  1,0,0, 0,1,0, 1,0,  0,0,1, 0,2,0, 0,1} idx = {1, 2, 3}\n";

int Channel(draw::Rgba c, int k) { return static_cast<int>((c >> (8 * k)) & 255); }

void TestMeshes() {
    SetMod("msh", std::string("sand = wum.draw.texture('sand.png') arr = wum.draw.texture('mat.png', {layers = 4})\n") + kTri);
    SetMod("msh2", std::string("mine = wum.draw.texture('x.png')\n") + kTri);
    Expect(sandbox::LoadMod("msh") && sandbox::LoadMod("msh2"), "mesh mods load");
    ExpectEq(Eval("msh", "return type(wum.draw.mesh), type(wum.draw.newMesh), type(wum.draw.freeMesh)"),
             "function\tfunction\tfunction", "mesh functions exist");
    draw::g_meshes.clear();
    draw::g_stage = render::Stage::Count;
    Expect(Eval("msh", "return wum.draw.mesh(verts, idx, sand)").find("only inside a wum.draw.on") != std::string::npos,
           "outside a draw callback raises");
    draw::g_stage = render::Stage::Hud;
    Expect(Eval("msh", "return wum.draw.mesh(verts, idx, sand)").rfind("ERR:", 0) == 0, "a hud callback raises");
    draw::g_stage = render::Stage::World;
    ++draw::g_frame;

    ExpectEq(Eval("msh", "return wum.draw.mesh(verts, idx, sand)"), "true", "vertex-array form");
    Expect(draw::g_meshes.size() == 1, "one mesh submitted");
    if (!draw::g_meshes.empty()) {
        const draw::MeshCall& c = draw::g_meshes.back();
        Expect(c.v.size() == 3 && c.idx.size() == 3 && c.idx[0] == 0 && c.idx[1] == 1 && c.idx[2] == 2, "1-based indices -> 0-based");
        Expect(c.v[1].pos[0] == 1 && c.v[2].pos[2] == 1 && c.v[1].uv[0] == 1 && c.v[2].uv[1] == 1, "positions and uv");
        ExpectEq(Eval("msh", "return sand"), std::to_string(c.texture), "texture passed through");
        Expect(c.flags == (draw::kDepthTest | draw::kDepthWrite), "defaults: depth test and write, culled, alpha blend");
        Expect(c.hasModel && c.model[0] == 1 && c.model[5] == 1 && c.model[10] == 1 && c.model[12] == 0, "nil model = identity");
        // Normal +Y under the default light (0, 0.936, -0.351): 0.6 + 0.4 * 0.9363 = 0.975 -> 248.6.
        const int r = Channel(c.v[0].color, 0);
        Expect(r >= 248 && r <= 250 && Channel(c.v[0].color, 3) == 255, "lit by the default light: " + std::to_string(r));
        Expect(c.v[2].color == c.v[0].color, "normals are normalised");
    }
    Eval("msh", "wum.draw.mesh(verts, idx, nil, nil, {lit = false})");
    Expect(draw::g_meshes.back().v[0].color == 0xffffffffu && draw::g_meshes.back().texture == 0, "unlit and untextured");
    Eval("msh", "wum.draw.mesh(verts, idx, 0, nil, {lit = false, color = '#ff800080'})");
    Expect(draw::g_meshes.back().v[0].color == 0x800080ffu, "opts.color tints the array form");
    Eval("msh", "wum.draw.mesh({0,0,0, 0,1,0, 0,0, 0.5,0.5,0.5,0.5,  1,0,0, 0,1,0, 1,0, 1,1,1,1,  0,0,1, 0,1,0, 0,1, 1,1,1,1},"
                " idx, nil, nil, {lit = false, format = 'pntc'})");
    Expect(Channel(draw::g_meshes.back().v[0].color, 0) == 128 && Channel(draw::g_meshes.back().v[0].color, 3) == 128 &&
               draw::g_meshes.back().v[1].color == 0xffffffffu,
           "pntc: per-vertex colour");
    Eval("msh", "wum.draw.mesh(verts, idx, nil, nil, {light = {0, -1, 0}, ambient = 0.2})");
    Expect(Channel(draw::g_meshes.back().v[0].color, 0) == 51, "a light from below leaves the ambient (0.2 -> 51)");
    Eval("msh", "wum.draw.mesh(verts, idx, nil, {pos = {1, 2, 3}, yaw = math.pi / 2, scale = 2}, {light = {1, 0, 0}, ambient = 0})");
    {
        const draw::MeshCall& c = draw::g_meshes.back();
        Expect(std::fabs(c.model[0]) < 1e-6f && std::fabs(c.model[2] + 2) < 1e-6f && std::fabs(c.model[8] - 2) < 1e-6f &&
                   c.model[5] == 2 && c.model[12] == 1 && c.model[13] == 2 && c.model[14] == 3,
               "model {pos, yaw, scale}: translate * rotate about +Y * scale");
        Expect(Channel(c.v[0].color, 0) == 0, "an up normal stays up under yaw (lit from +X: dark)");
    }
    Eval("msh", "wum.draw.mesh({0,0,0, 0,0,1, 0,0,  1,0,0, 0,0,1, 1,0,  0,1,0, 0,0,1, 0,1}, idx, nil,"
                " {yaw = math.pi / 2}, {light = {1, 0, 0}, ambient = 0})");
    Expect(Channel(draw::g_meshes.back().v[0].color, 0) == 255, "a +Z normal turned by yaw pi/2 faces +X");
    Eval("msh", "wum.draw.mesh(verts, idx, nil, {scale = {1, 4, 1}}, {light = {0, 1, 0}, ambient = 0})");
    Expect(Channel(draw::g_meshes.back().v[0].color, 0) == 255, "non-uniform scale keeps an up normal up");
    Eval("msh", "wum.draw.mesh(verts, idx, nil, {1,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1})");
    Expect(draw::g_meshes.back().model[12] == 5 && draw::g_meshes.back().model[14] == 7, "a 16-number model matrix");
    Eval("msh", "wum.draw.mesh(verts, idx, nil, nil, {blend = 'additive', cull = false, depthWrite = false, xray = true})");
    Expect(draw::g_meshes.back().flags == (draw::kAdditive | draw::kNoCull), "additive, two-sided, no depth test or write");

    // Persistent meshes.
    ExpectEq(Eval("msh", "h = wum.draw.newMesh(verts, idx) return math.type(h)"), "integer", "newMesh returns an integer handle");
    ExpectEq(Eval("msh", "return wum.draw.mesh(h, sand, {pos = {0, 10, 0}}, '#ff000080')"), "true", "handle form");
    {
        const draw::MeshCall& c = draw::g_meshes.back();
        Expect(c.v.size() == 3 && c.model[13] == 10 && Channel(c.v[0].color, 1) == 0 && Channel(c.v[0].color, 3) == 128,
               "handle form: model and tint");
    }
    Eval("msh", "wum.draw.mesh(h, nil, nil, nil, {lit = false, blend = 'additive'})");
    Expect(draw::g_meshes.back().v[0].color == 0xffffffffu && (draw::g_meshes.back().flags & draw::kAdditive),
           "handle form options");
    const size_t before = draw::g_meshes.size();
    const char* bad[] = {
        "wum.draw.mesh(verts, {1, 2, 4}, sand)",                     // index past the end
        "wum.draw.mesh(verts, {0, 1, 2}, sand)",                     // zero index
        "wum.draw.mesh(verts, {1, 2}, sand)",                        // not a triangle list
        "wum.draw.mesh(verts, {1, 2, 2.5}, sand)",                   // not an integer
        "wum.draw.mesh({0, 0, 0, 0}, idx, sand)",                    // not 8 numbers per vertex
        "wum.draw.mesh({0,0,0, 0,1,0, 0,0/0, 1,0,0, 0,1,0, 1,0, 0,0,1, 0,1,0, 0,1}, idx)",  // NaN
        "wum.draw.mesh(verts, idx, 'sand')",                         // texture type
        "wum.draw.mesh(verts, idx, 9999)",                           // never loaded
        "wum.draw.mesh(verts, idx, arr)",                            // texture array
        "wum.draw.mesh(verts, idx, nil, 5)",                         // model type
        "wum.draw.mesh(verts, idx, nil, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})",  // singular
        "wum.draw.mesh(verts, idx, nil, nil, {glow = true})",        // unknown option
        "wum.draw.mesh(verts, idx, nil, nil, {blend = 'add'})",      // blend
        "wum.draw.mesh(verts, idx, nil, nil, {lit = 1})",            // boolean option
        "wum.draw.mesh(verts, idx, nil, nil, {format = 'pn'})",      // format
        "wum.draw.mesh(verts, idx, nil, nil, {light = {0, 0, 0}})",  // zero light
        "wum.draw.mesh(h, nil, nil, nil, {color = '#ffffff'})",      // color is the 4th argument in the handle form
        "wum.draw.mesh(h + 100, sand)",                              // unknown handle
        "wum.draw.newMesh(verts, idx, 'xyz')",                       // format
    };
    for (const char* code : bad) Expect(Eval("msh", code).rfind("ERR:", 0) == 0, std::string("raises: ") + code);
    const std::string foreign = Eval("msh2", "return wum.draw.mesh(verts, idx, " + Eval("msh", "return sand") + ")");
    Expect(foreign.find("texture id from this mod's wum.draw.texture") != std::string::npos, "another mod's texture is refused");
    Expect(Eval("msh2", "return wum.draw.mesh(" + Eval("msh", "return h") + ", nil)").rfind("ERR:", 0) == 0,
           "another mod's mesh handle is refused");
    Expect(draw::g_meshes.size() == before, "nothing submitted by a failed call");
    ExpectEq(Eval("msh", "return wum.draw.freeMesh(h), wum.draw.freeMesh(h)"), "true\tfalse", "freeMesh");
    Expect(Eval("msh", "return wum.draw.mesh(h, nil)").rfind("ERR:", 0) == 0, "a freed handle is refused");

    // Caps: 64 live meshes per mod; 256 mesh calls per mod and frame.
    ExpectEq(Eval("msh", "local n = 0 for i = 1, 70 do if wum.draw.newMesh(verts, idx) then n = n + 1 end end "
                         "return n, select(2, wum.draw.newMesh(verts, idx))"),
             "64\ttoo many meshes", "64 meshes at most");
    ++draw::g_frame;
    ExpectEq(Eval("msh", "local n = 0 for i = 1, 300 do if wum.draw.mesh(2, nil) then n = n + 1 end end return n"), "256",
             "256 mesh calls per frame");
    ExpectEq(Eval("msh2", "return wum.draw.mesh(verts, idx)"), "true", "the frame cap is per mod");
    ++draw::g_frame;
    ExpectEq(Eval("msh", "return wum.draw.mesh(2, nil)"), "true", "the next frame has a fresh budget");
    Expect(sandbox::ReloadMod("msh"), "mesh mod reloads");
    Expect(Eval("msh", "return wum.draw.mesh(2, nil)").rfind("ERR:", 0) == 0, "a reload frees the previous version's meshes");

    draw::g_stage = render::Stage::Count;
    draw::g_meshes.clear();
    sandbox::UnloadMod("msh");
    sandbox::UnloadMod("msh2");
}

void TestCameraOverride() {
    SetMod("cam", "");
    SetMod("cam2", "");
    Expect(sandbox::LoadMod("cam") && sandbox::LoadMod("cam2"), "camera mods load");
    render::g_ovOwner = nullptr;
    render::g_ovResult = render::OverrideResult::Ok;
    ExpectEq(Eval("cam", "return wum.render.cameraOverride({pos = {0, 100, 300}, target = {0, 0, 0}, fov = 40, frames = 5})"), "true",
             "an override is set");
    Expect(render::g_ovOwner && render::g_ov.pos[2] == 300 && render::g_ov.target[1] == 0 && render::g_ov.up[1] == 1 &&
               render::g_ov.fovDeg == 40 && render::g_ovFrames == 5,
           "spec passed through; up defaults to +Y");
    ExpectEq(Eval("cam", "return wum.render.cameraOverride({pos = {1, 2, 3}, target = {x = 4, y = 5, z = 6}, up = {0, 0, 1}})"),
             "true", "the owner refreshes it");
    Expect(render::g_ov.target[2] == 6 && render::g_ov.up[2] == 1 && render::g_ov.fovDeg == 0 && render::g_ovFrames == 0,
           "x/y/z fields; fov and frames default to the game's / 30");
    ExpectEq(Eval("cam2", "return wum.render.cameraOverride({pos = {0, 1, 0}, target = {0, 0, 0}})"), "false\tbusy",
             "another mod's override is busy");
    ExpectEq(Eval("cam2", "return wum.render.cameraOverride(nil)"), "false", "another mod cannot clear it");
    const int sets = render::g_ovSets;
    const char* bad[] = {
        "wum.render.cameraOverride({target = {0, 0, 0}})",                              // no pos
        "wum.render.cameraOverride({pos = {0, 0, 0}})",                                 // no target
        "wum.render.cameraOverride({pos = {1, 1, 1}, target = {1, 1, 1}})",             // same point
        "wum.render.cameraOverride({pos = {0, 0/0, 0}, target = {0, 0, 0}})",           // NaN
        "wum.render.cameraOverride({pos = {0, 2e6, 0}, target = {0, 0, 0}})",           // out of range
        "wum.render.cameraOverride({pos = {0, 1, 0}, target = {0, 0, 0}, fov = 200})",  // fov
        "wum.render.cameraOverride({pos = {0, 1, 0}, target = {0, 0, 0}, fov = 'x'})",  // fov type
        "wum.render.cameraOverride({pos = {0, 1, 0}, target = {0, 0, 0}, frames = 0})",  // frames
        "wum.render.cameraOverride({pos = {0, 1, 0}, target = {0, 0, 0}, frames = 601})",
        "wum.render.cameraOverride({pos = {0, 1, 0}, target = {0, 0, 0}, roll = 1})",  // unknown field
        "wum.render.cameraOverride({pos = 5, target = {0, 0, 0}})",                     // pos type
        "wum.render.cameraOverride(5)",
    };
    for (const char* code : bad) Expect(Eval("cam", code).rfind("ERR:", 0) == 0, std::string("raises: ") + code);
    Expect(render::g_ovSets == sets, "nothing set by a failed call");

    render::g_haveCam = true;
    render::g_cam = {};
    render::g_cam.proj[5] = 1.f / std::tan(30.f * 3.14159265f / 180.f);  // 60 degrees vertical
    render::g_cam.proj[11] = -1.f;
    render::g_cam.pos[1] = 100;
    render::g_cam.valid = true;
    render::g_gameCam = render::g_cam;
    render::g_gameCam.pos[1] = 7;
    ExpectEq(Eval("cam", "local c = wum.render.camera() return math.floor(c.fov + 0.5), c.overridden, c.pos.y"), "60\ttrue\t100",
             "camera() reports fov and the override flag");
    ExpectEq(Eval("cam", "return wum.render.gameCamera().pos.y"), "7", "gameCamera() reports the game's own camera");
    ExpectEq(Eval("cam", "return wum.render.cameraOverride(nil), wum.render.cameraOverride()"), "true\tfalse",
             "nil clears the mod's own override once");
    ExpectEq(Eval("cam", "return wum.render.camera().overridden"), "false", "camera() after the clear");
    render::g_haveCam = false;
    ExpectEq(Eval("cam", "return wum.render.camera() == nil, wum.render.gameCamera() == nil"), "true\ttrue",
             "no camera outside rendering");

    ExpectEq(Eval("cam2", "return wum.render.cameraOverride({pos = {0, 1, 0}, target = {0, 0, 0}})"), "true", "another mod takes it");
    sandbox::UnloadMod("cam2");
    Expect(render::g_ovOwner == nullptr, "unloading the mod clears its override");
    render::g_ovResult = render::OverrideResult::Unavailable;
    ExpectEq(Eval("cam", "return wum.render.cameraOverride({pos = {0, 1, 0}, target = {0, 0, 0}})"), "false\tunavailable",
             "unavailable (no match, unknown build)");
    render::g_ovResult = render::OverrideResult::Ok;
    sandbox::UnloadMod("cam");
}

void TestShaderTextures() {
    SetMod("shtex", "plain = wum.draw.texture('plain.png')\n"
                    "arr = wum.draw.texture('mat.png', {wrap = 'repeat', filter = 'linear', layers = 4})\n"
                    "lut = wum.draw.texture('lut.png', {filter = 'nearest'})\n");
    SetMod("shtex2", "mine = wum.draw.texture('x.png')\n");
    Expect(sandbox::LoadMod("shtex") && sandbox::LoadMod("shtex2"), "shader texture mods load");
    ExpectEq(Eval("shtex", "return type(wum.shaders.setTexture)"), "function", "capability flag: wum.shaders.setTexture exists");
    ExpectEq(Eval("shtex", "local o = {} return lut ~= nil and arr ~= nil"), "true", "option textures load");
    Expect(!draw::g_lastOpts.repeat && draw::g_lastOpts.nearest && draw::g_lastOpts.layers == 0, "nearest, clamp, 2D");
    Eval("shtex", "arr2 = wum.draw.texture('mat.png', {wrap = 'repeat', layers = 8})");
    Expect(draw::g_lastOpts.repeat && !draw::g_lastOpts.nearest && draw::g_lastOpts.layers == 8, "repeat, linear, 8 layers");
    ExpectEq(Eval("shtex", "local t, e = wum.draw.texture('odd.png', {layers = 3}) return tostring(t) .. ' ' .. e"),
             "nil cannot load odd.png: image height is not a multiple of layers", "the loader's reason reaches Lua");
    const char* badOpts[] = {
        "wum.draw.texture('a.png', {wrap = 'mirror'})",  "wum.draw.texture('a.png', {filter = 'cubic'})",
        "wum.draw.texture('a.png', {layers = 0})",       "wum.draw.texture('a.png', {layers = 65})",
        "wum.draw.texture('a.png', {layers = 1.5})",     "wum.draw.texture('a.png', {mips = false})",
        "wum.draw.texture('a.png', 'repeat')",
    };
    for (const char* code : badOpts) Expect(Eval("shtex", code).rfind("ERR:", 0) == 0, std::string("raises: ") + code);

    ExpectEq(Eval("shtex", "return wum.shaders.setTexture('FixedFunction.cg', 'FFFragmentMainTexLit*', 'materials', arr)"), "true",
             "an own replacement's sampler is set");
    ExpectEq(shaders::g_texSampler + " " + std::to_string(shaders::g_texSet), "materials " + Eval("shtex", "return arr"),
             "sampler and texture id passed through");
    ExpectEq(Eval("shtex", "return wum.shaders.setTexture('FixedFunction.cg', 'FFFragmentMainTexLit', 'materials', nil)"), "true",
             "nil unsets");
    Expect(shaders::g_texSet == 0, "nil passes texture 0");
    Expect(Eval("shtex", "wum.shaders.setTexture('FixedFunction.cg', 'X', 'materials', 9999)").rfind("ERR:", 0) == 0, "an unknown id raises");
    Expect(Eval("shtex", "wum.shaders.setTexture('FixedFunction.cg', 'X', 'materials', " + Eval("shtex2", "return mine") + ")").rfind("ERR:", 0) == 0,
           "another mod's texture id raises");
    Expect(Eval("shtex", "wum.shaders.setTexture('FixedFunction.cg', 'X', 'mg_scene', arr)").rfind("ERR:", 0) == 0, "a reserved name raises");
    Expect(Eval("shtex2", "wum.shaders.setTexture('FixedFunction.cg', 'X', 'materials', mine)").rfind("ERR:", 0) == 0,
           "another mod's replacement raises");
    shaders::g_texInactive = true;
    ExpectEq(Eval("shtex", "return wum.shaders.setTexture('FixedFunction.cg', 'X', 'materials', arr)"), "false",
             "false while GLSL replacements are off");
    shaders::g_texInactive = false;

    draw::g_stage = render::Stage::World;
    Expect(Eval("shtex", "return wum.draw.sprite(arr, 0, 0, 0, 1)").find("texture array") != std::string::npos, "an array is no sprite");
    ExpectEq(Eval("shtex", "return wum.draw.sprite(lut, 0, 0, 0, 1)"), "true", "a 2D option texture can be a sprite");
    draw::g_stage = render::Stage::Count;
    draw::g_sprites.clear();

    const unsigned arr = static_cast<unsigned>(std::stoul(Eval("shtex", "return arr")));
    shaders::g_texForgotten.clear();
    sandbox::UnloadMod("shtex");
    Expect(std::find(shaders::g_texForgotten.begin(), shaders::g_texForgotten.end(), arr) != shaders::g_texForgotten.end(),
           "unload drops the shader bindings of the mod's textures");
    sandbox::UnloadMod("shtex2");
}

void TestInput() {
    fake::g_ctlAvail = true;
    fake::g_ctlOpts = controls::Options{};
    fake::g_ctlOwner = nullptr;
    SetMod("inp", R"lua(
groups = wum.input.groups()
bind = wum.input.binding("Worm.Jump")
nobind = wum.input.binding("Nope")
r1 = wum.input.setOptions({ cameraInvertY = "standard", aimInvertY = "inverted", blimpInvert = true,
                            cameraSensitivity = 9, aimSensitivity = 0.5 })
o = wum.input.options()
badMode = pcall(wum.input.setOptions, { cameraInvertY = "sideways" })
badSens = pcall(wum.input.setOptions, { aimSensitivity = "fast" })
badBlimp = pcall(wum.input.setOptions, { blimpInvert = 1 })
ok1, e1 = pcall(wum.ui.hotkey, "F5", function() end)
ok2, e2 = pcall(wum.ui.hotkey, "Q", function() end)
)lua");
    Expect(sandbox::LoadMod("inp"), "inp loads: " + StatusOf("inp").error);
    ExpectEq(Eval("inp", "return #groups .. groups[1] .. groups[2]"), "2InGameWormFirstPersonAiming", "groups() lists active groups in order");
    ExpectEq(Eval("inp", "return bind"), "Space", "binding() returns a label");
    ExpectEq(Eval("inp", "return tostring(nobind)"), "nil", "binding() of an unknown message is nil");
    ExpectEq(Eval("inp", "return r1"), "true", "setOptions returns true");
    ExpectEq(Eval("inp", "return o.cameraInvertY .. o.aimInvertY .. tostring(o.blimpInvert)"), "standardinvertedtrue", "options() reflects setOptions");
    ExpectEq(Eval("inp", "return o.cameraSensitivity"), "3", "cameraSensitivity is clamped to 3.0");
    ExpectEq(Eval("inp", "return o.aimSensitivity"), "0.5", "aimSensitivity 0.5");
    ExpectEq(Eval("inp", "return tostring(o.smoothMouse)"), "true", "options() reports smoothMouse");
    ExpectEq(Eval("inp", "return badMode or badSens or badBlimp"), "false", "bad option values raise");
    ExpectEq(Eval("inp", "return ok1"), "true", "wum.ui.hotkey accepts a bare F5");
    ExpectEq(Eval("inp", "return ok2"), "false", "wum.ui.hotkey still wants a modifier for Q");
    Expect(fake::g_ctlOpts.cameraInvertY == controls::InvertMode::Standard && fake::g_ctlOpts.blimpInvert, "options reached the module");
    sandbox::UnloadMod("inp");
    Expect(fake::g_ctlOpts.cameraInvertY == controls::InvertMode::Game && !fake::g_ctlOpts.blimpInvert && fake::g_ctlOpts.aimSensitivity == 1.0f,
           "options are cleared when the owning mod unloads");

    // Last caller wins: a second mod takes over; the first one unloading then leaves the second's options alone.
    SetMod("inpA", "wum.input.setOptions({ aimSensitivity = 2 })");
    SetMod("inpB", "wum.input.setOptions({ cameraSensitivity = 1.5 })");
    Expect(sandbox::LoadMod("inpA") && sandbox::LoadMod("inpB"), "two input mods load");
    Expect(fake::g_ctlOpts.cameraSensitivity == 1.5f && fake::g_ctlOpts.aimSensitivity == 1.0f, "last caller wins");
    sandbox::UnloadMod("inpA");
    Expect(fake::g_ctlOpts.cameraSensitivity == 1.5f, "an earlier caller unloading does not clear a later caller's options");
    ExpectEq(Eval(nullptr, "wum.input.setOptions(nil) return wum.input.options().cameraSensitivity"), "1", "setOptions(nil) clears");
    sandbox::UnloadMod("inpB");

    fake::g_ctlAvail = false;
    ExpectEq(Eval(nullptr, "local a, b = wum.input.groups() return tostring(a) .. ':' .. tostring(b)"), "nil:unavailable", "groups() is unavailable without the module");
    fake::g_ctlAvail = true;
}

std::string TestWavBytes(uint32_t rate, uint32_t frames) {
    auto u32 = [](uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [](uint16_t v) { return std::string(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = frames * 2;
    return "RIFF" + u32(36 + bytes) + "WAVEfmt " + u32(16) + u16(1) + u16(1) + u32(rate) + u32(rate * 2) + u16(2) + u16(16) + "data" +
           u32(bytes) + std::string(bytes, '\x10');
}

void TestAudio() {
    fake::g_audioAvail = true;
    fake::g_audioVoices.clear();
    fake::g_audioClips.clear();
    fake::g_audioReleases = 0;
    SetMod("aud", R"lua(
h = wum.audio.load("sfx/boom.wav")
h2 = wum.audio.load("SFX\\Boom.wav")
h3 = wum.audio.load("sfx/./boom.wav")
nofile, nofileWhy = wum.audio.load("sfx/missing.wav")
badpath, badpathWhy = wum.audio.load("../x.wav")
abspath, abspathWhy = wum.audio.load("C:\\Windows\\x.wav")
junk, junkWhy = wum.audio.load("sfx/junk.wav")
big, bigWhy = wum.audio.load("sfx/big.wav")
v1 = wum.audio.play(h)
v2 = wum.audio.play(h, { volume = math.huge, pitch = -math.huge, loop = true, pos = { 1, 2, 3 } })
v3 = wum.audio.play(h, { volume = 0.25, pos = { x = 4, y = 5, z = 6 } })
v4 = wum.audio.play(h, { pos = { 1e300, 1 / 0, -1 / 0 } })
nov, novWhy = wum.audio.play(9999)
okOpts, errOpts = pcall(wum.audio.play, h, 5)
okVol = pcall(wum.audio.play, h, { volume = "loud" })
okLoop = pcall(wum.audio.play, h, { loop = 1 })
okPos = pcall(wum.audio.play, h, { pos = 3 })
okPosC, errPosC = pcall(wum.audio.play, h, { pos = { 1, "up", 3 } })
okHandle = pcall(wum.audio.play, "boom")
okLoad = pcall(wum.audio.load, {})
)lua");
    const std::wstring dir = fake::g_modsDir + L"\\aud";
    WriteText(dir + L"\\sfx\\boom.wav", TestWavBytes(22050, 100));
    WriteText(dir + L"\\sfx\\junk.wav", "this is not a wav file at all");
    WriteText(dir + L"\\sfx\\big.wav", std::string((2u << 20) + 2, '\0'));
    Expect(sandbox::LoadMod("aud"), "aud loads: " + StatusOf("aud").error);

    ExpectEq(Eval("aud", "return math.type(h)"), "integer", "load returns an integer handle");
    ExpectEq(Eval("aud", "return tostring(h == h2 and h == h3)"), "true", "equivalent spellings of one path share a handle");
    size_t clips = 0;
    for (const auto& [owner, cache] : fake::g_audioClips) clips += cache.Count();
    Expect(clips == 1, "three spellings cost one clip");
    ExpectEq(Eval("aud", "return tostring(nofile) .. ':' .. nofileWhy"), "nil:cannot open file", "a missing file is nil plus a reason");
    ExpectEq(Eval("aud", "return tostring(badpath) .. ':' .. badpathWhy"), "nil:path must be relative and inside the mod folder", "'..' is refused");
    ExpectEq(Eval("aud", "return tostring(abspath)"), "nil", "an absolute path is refused");
    Expect(Eval("aud", "return tostring(junk) .. ':' .. junkWhy").rfind("nil:", 0) == 0, "a file that is not a WAV is nil plus the parser's reason");
    ExpectEq(Eval("aud", "return tostring(big) .. ':' .. bigWhy"), "nil:too large", "an oversized file is 'too large'");

    ExpectEq(Eval("aud", "return math.type(v1)"), "integer", "play returns an integer voice id");
    ExpectEq(Eval("aud", "return tostring(nov) .. ':' .. novWhy"), "nil:unknown sound", "an unknown handle is nil plus a reason");
    uint32_t id1 = 0, id2 = 0, id3 = 0, id4 = 0;
    for (const auto& [id, v] : fake::g_audioVoices) {
        if (!v.opts.positional) id1 = id;
        else if (v.opts.loop) id2 = id;
        else if (v.opts.volume == 0.25f) id3 = id;
        else id4 = id;
    }
    Expect(id1 && id2 && id3 && id4 && fake::g_audioVoices.size() == 4, "four voices started");
    const melange::audio::PlayOpts& o1 = fake::g_audioVoices[id1].opts;
    Expect(o1.volume == 1.0f && o1.pitch == 1.0f && !o1.loop, "defaults: volume 1, pitch 1, no loop");
    const melange::audio::PlayOpts& o2 = fake::g_audioVoices[id2].opts;
    Expect(o2.volume == 2.0f && o2.pitch == 0.5f, "infinite volume and pitch clamp to the range ends");
    Expect(o2.pos[0] == 1.0f && o2.pos[1] == 2.0f && o2.pos[2] == 3.0f, "pos as an array");
    const melange::audio::PlayOpts& o3 = fake::g_audioVoices[id3].opts;
    Expect(o3.pos[0] == 4.0f && o3.pos[1] == 5.0f && o3.pos[2] == 6.0f, "pos as x, y, z fields");
    const melange::audio::PlayOpts& o4 = fake::g_audioVoices[id4].opts;
    Expect(o4.pos[0] == FLT_MAX && o4.pos[1] == FLT_MAX && o4.pos[2] == -FLT_MAX, "out-of-range pos components clamp to the float range");

    ExpectEq(Eval("aud", "return tostring(okOpts or okVol or okLoop or okPos or okPosC or okHandle or okLoad)"), "false", "wrong argument types raise");
    Expect(Eval("aud", "return errOpts").find("table") != std::string::npos, "the opts error names the problem");
    Expect(Eval("aud", "return errPosC").find("pos.y") != std::string::npos, "the pos error names the component");
    ExpectEq(Eval("aud", "return tostring(wum.audio.ready())"), "true", "ready() reports the backend");

    // Voices are scoped to the mod that started them.
    SetMod("aud2", "other = wum.audio.load('x.wav')");
    Expect(sandbox::LoadMod("aud2"), "second audio mod loads");
    ExpectEq(Eval("aud2", "return tostring(wum.audio.stop(" + std::to_string(id1) + "))"), "false", "another mod's voice cannot be stopped");
    ExpectEq(Eval("aud2", "return wum.audio.stopAll()"), "0", "stopAll counts only this mod's voices");
    Expect(fake::g_audioVoices.size() == 4, "the other mod's voices are untouched");
    ExpectEq(Eval("aud", "return tostring(wum.audio.stop(" + std::to_string(id1) + "))"), "true", "a mod stops its own voice");
    ExpectEq(Eval("aud", "return tostring(wum.audio.stop(" + std::to_string(id1) + "))"), "false", "a stopped voice is gone");
    ExpectEq(Eval("aud", "return tostring(wum.audio.stop(99999))"), "false", "an unknown voice is false");
    ExpectEq(Eval("aud", "return wum.audio.stopAll()"), "3", "stopAll returns how many it stopped");

    // No output device: loading still works, playing says why.
    fake::g_audioAvail = false;
    ExpectEq(Eval("aud", "return tostring(wum.audio.ready())"), "false", "ready() is false without a device");
    ExpectEq(Eval("aud", "local a = wum.audio.load('sfx/boom.wav') return math.type(a)"), "integer", "load needs no device");
    ExpectEq(Eval("aud", "local a, b = wum.audio.play(h) return tostring(a) .. ':' .. b"), "nil:audio unavailable", "play without a device is nil plus a reason");
    fake::g_audioAvail = true;

    // Unload releases the mod's voices and clips.
    ExpectEq(Eval("aud", "return tostring(wum.audio.play(h) ~= nil)"), "true", "a voice to be released");
    const int releases = fake::g_audioReleases;
    sandbox::UnloadMod("aud");
    Expect(fake::g_audioReleases > releases, "unload releases the mod's audio");
    Expect(fake::g_audioVoices.empty(), "unload stops the mod's voices");
    sandbox::UnloadMod("aud2");
    fake::g_audioClips.clear();
}

// Owner-only events (QueueEventFor, lua::PostEventTo): only the target mod's listeners run.
void TestOwnerEvents() {
    SetMod("ownA", "got = 0 wum.events.on('melange.owned', function(p) got = got + p.n end)");
    SetMod("ownB", "got = 0 wum.events.on('melange.owned', function(p) got = got + p.n end)");
    Expect(sandbox::LoadMod("ownA") && sandbox::LoadMod("ownB"), "owner-event mods load");
    json::Value v;
    v.type = json::Type::Object;
    json::Value n;
    n.type = json::Type::Number;
    n.number = 2;
    v.members.emplace_back("n", n);
    sandbox::QueueEventFor("ownA", "melange.owned", v);
    sandbox::QueueEventFor("", "melange.owned", v);  // no owner: dropped, never broadcast
    Expect(lua::PostEventTo("ownB", "melange.owned", "{\"n\": 5}"), "PostEventTo accepted");
    Expect(!lua::PostEventTo("", "melange.owned", "{}"), "PostEventTo needs a mod id");
    Frames(1);
    ExpectEq(Eval("ownA", "return got"), "2", "QueueEventFor reaches its mod only");
    ExpectEq(Eval("ownB", "return got"), "5", "PostEventTo reaches its mod only");
    sandbox::UnloadMod("ownA");
    sandbox::UnloadMod("ownB");
}

// Melange 0.12 (Holo): one cases file per namespace owner.
#include "sandbox_cases_media.inc"
#include "sandbox_cases_peer.inc"
#include "sandbox_cases_input.inc"
#include "sandbox_cases_draw.inc"
#include "sandbox_cases_audio.inc"
#include "sandbox_cases_game.inc"

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
        {"game", TestGame},       {"match", TestMatch},     {"graphics", TestGraphics}, {"sprites", TestSprites},
        {"meshes", TestMeshes},   {"camera override", TestCameraOverride},
        {"shader textures", TestShaderTextures},
        {"input", TestInput}, {"audio", TestAudio},
        {"owner events", TestOwnerEvents}, {"media", TestMedia}, {"peer", TestPeer}, {"mod input", TestModInput},
        {"draw screen", TestDrawScreen}, {"music", TestMusic}, {"game turn", TestGameTurn},
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
