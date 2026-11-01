// wum.spotify (Melange 0.13.0): Spotify for client mods. Presentation only, in the client Sandbox VM. The work is done
// by src/spotify/* through host.h; this file validates every argument (spotify_logic.h) before a request is queued,
// checks the click that connect() needs, and scopes request callbacks to the calling mod generation. A generation's
// one cleanup cancels its waiting requests and never pauses the music.
//
// Melange 0.14.0 adds the in-game setup (setup, setClientId, pasteClientId, copy, openDashboard), cancel and transfer;
// they always ship together, so a mod tests `wum.spotify.setup` once. The click rule (input::gesture, consent-grade,
// within 2 s) is shared: connect() and openDashboard() consume the click; setClientId(), pasteClientId() and copy() need
// one but leave it, so one click can paste, save and connect. Lua never sees clipboard text other than a Client ID found
// there, and never the whole Client ID in use.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "input/gesture.h"
#include "lua/event_names.h"
#include "lua/sandbox_core.h"
#include "spotify/host.h"
#include "spotify/spotify_logic.h"

namespace melange::sandbox {
namespace {
namespace host = melange::spotify::host;
namespace logic = melange::spotify::logic;

constexpr uint32_t kGestureMs = 2000;          // the click (connect, setClientId, ...) must be this recent
constexpr int64_t kMaxPositionMs = 0x7fffffff;  // positionMs and seek(ms)
constexpr int64_t kMaxOffset = 100000000;       // library offsets
constexpr size_t kMaxStr = 1024;                // a longer string field is never valid

int Fail(lua_State* L, const char* why) {
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}
int Fail(lua_State* L, const std::string& why) { return Fail(L, why.c_str()); }
int Invalid(lua_State* L) { return Fail(L, "invalid"); }
int True(lua_State* L) {
    lua_pushboolean(L, true);
    return 1;
}

ModRec* NeedMod(lua_State* L, const char* fn) {
    ModRec* m = Current();
    if (!m || !CurrentGen()) luaL_error(L, "wum.spotify.%s needs a mod context (not available in the console environment)", fn);
    return m;
}

// ---------------------------------------------------------------- delivery from the Spotify host
class LuaSink final : public host::Sink {
public:
    void Event(const std::string& mod, const json::Value& payload) override { QueueEventFor(mod, ev::kSpotify, payload); }
    void Callback(const std::string& mod, uint32_t token, const json::Value& arg, bool done) override {
        sandbox::Callback* cb = FindCallback(token);
        if (CallbackLive(cb) && cb->kind == CbKind::Spotify && cb->gen->mod->id == mod)
            Invoke(cb, [&arg](lua_State* L) {
                PushJson(L, arg);
                return 1;
            });
        if (done && cb) KillCallback(token);
    }
};
LuaSink g_sink;

// ---------------------------------------------------------------- argument helpers
// A number that is an integer in lo..hi (NaN, inf and fractions are not).
bool IntValue(lua_State* L, int idx, int64_t lo, int64_t hi, int64_t* out) {
    int64_t v = 0;
    if (lua_isinteger(L, idx)) {
        v = static_cast<int64_t>(lua_tointeger(L, idx));
    } else {
        const double d = lua_tonumber(L, idx);
        if (!std::isfinite(d) || std::floor(d) != d || d < static_cast<double>(lo) || d > static_cast<double>(hi)) return false;
        v = static_cast<int64_t>(d);
    }
    if (v < lo || v > hi) return false;
    *out = v;
    return true;
}

enum class Got { Absent, Ok, Invalid };
// opts.k as an integer in lo..hi. nil: Absent; another type raises; a bad number is Invalid.
Got IntField(lua_State* L, int idx, const char* fn, const char* k, int64_t lo, int64_t hi, int64_t* out) {
    const int t = lua_getfield(L, idx, k);
    Got g = Got::Absent;
    if (t != LUA_TNIL) {
        if (t != LUA_TNUMBER) luaL_error(L, "wum.spotify.%s: opts.%s must be a number", fn, k);
        g = IntValue(L, -1, lo, hi, out) ? Got::Ok : Got::Invalid;
    }
    lua_pop(L, 1);
    return g;
}
// opts.k as a string. nil: Absent; another type raises; longer than kMaxStr is Invalid.
Got StrField(lua_State* L, int idx, const char* fn, const char* k, std::string* out) {
    const int t = lua_getfield(L, idx, k);
    Got g = Got::Absent;
    if (t != LUA_TNIL) {
        if (t != LUA_TSTRING) luaL_error(L, "wum.spotify.%s: opts.%s must be a string", fn, k);
        size_t n = 0;
        const char* s = lua_tolstring(L, -1, &n);
        if (n > kMaxStr) {
            g = Got::Invalid;
        } else {
            out->assign(s, n);
            g = Got::Ok;
        }
    }
    lua_pop(L, 1);
    return g;
}
// An optional callback at idx: a function, or nil/none (0). Another type raises.
int OptFn(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return 0;
    luaL_checktype(L, idx, LUA_TFUNCTION);
    return idx;
}

// The calling mod after the type checks: touched, and Spotify available (else *ret holds nil, reason).
bool Ready(lua_State* L, ModRec* m, int* ret) {
    host::Touch(m->id);
    std::string why;
    if (host::Available(&why)) return true;
    *ret = Fail(L, why);
    return false;
}

// Queues `r` for the calling mod; the callback at fnIdx (0: none) gets the answer once.
int SubmitReq(lua_State* L, ModRec* m, const logic::Request& r, int fnIdx, const char* label) {
    Callback* cb = nullptr;
    if (fnIdx) {
        cb = NewCallback(L, fnIdx, CbKind::Spotify, label);
        Activate(cb);
    }
    std::string why;
    Gen* g = CurrentGen();
    const uint32_t rid = host::Submit(m->id, g->serial, r, cb ? cb->id : 0, &why);
    if (!rid) {
        if (cb) KillCallback(cb->id);
        return Fail(L, why);
    }
    if (!g->spotifyCleanup) {  // one per generation, not one per request: a mod that polls for hours adds nothing
        g->spotifyCleanup = true;
        g->cleanups.push_back([mod = m->id, serial = g->serial] { host::CancelGen(mod, serial); });  // never pauses the music
    }
    lua_pushinteger(L, static_cast<lua_Integer>(rid));
    return 1;
}

// ---------------------------------------------------------------- wum.spotify.*
int LAvailable(lua_State* L) {
    if (ModRec* m = Current(); m && CurrentGen()) host::Touch(m->id);
    std::string why;
    if (host::Available(&why)) return True(L);
    lua_pushboolean(L, false);
    lua_pushstring(L, why.c_str());
    return 2;
}

// From the console (no mod) it reports the global state.
int LStatus(lua_State* L) {
    std::string mod;
    if (ModRec* m = Current(); m && CurrentGen()) {
        mod = m->id;
        host::Touch(mod);
    }
    const host::Status st = host::GetStatus(mod);
    lua_createtable(L, 0, 6);
    lua_pushstring(L, st.state.c_str());
    lua_setfield(L, -2, "state");
    if (!st.user.empty()) {
        lua_pushlstring(L, st.user.data(), st.user.size());
        lua_setfield(L, -2, "user");
    }
    if (st.premium) {  // nil while unknown
        lua_pushboolean(L, *st.premium);
        lua_setfield(L, -2, "premium");
    }
    lua_pushboolean(L, st.linked);
    lua_setfield(L, -2, "linked");
    if (!st.error.empty()) {
        lua_pushstring(L, st.error.c_str());
        lua_setfield(L, -2, "error");
    }
    if (!st.app.empty()) {
        lua_pushstring(L, st.app.c_str());
        lua_setfield(L, -2, "app");
    }
    return 1;
}

// The browser sign-in, or the linked account granted to this mod: a consent-grade click on one of this mod's regions in
// the last 2 s is the user's consent (like wum.media.install).
int LConnect(lua_State* L) {
    ModRec* m = NeedMod(L, "connect");
    host::Touch(m->id);
    std::string why;
    if (!host::Available(&why)) return Fail(L, why);
    if (host::Granted(m->id)) return True(L);
    if (!input::gesture::Recent(m->id.c_str(), kGestureMs)) return Fail(L, "gesture");
    if (!host::Connect(m->id, &why)) return Fail(L, why);
    input::gesture::Clear(m->id.c_str());  // one click, one sign-in
    SysLog(1, "spotify sign-in requested (user click)", m->id);
    return True(L);
}

// A consent-grade click on one of this mod's regions in the last 2 s (not consumed here).
bool Clicked(const ModRec* m) { return input::gesture::Recent(m->id.c_str(), kGestureMs, true); }

// The module runs (a missing Client ID does not matter for the setup functions); else *ret holds nil, reason.
bool Up(lua_State* L, int* ret) {
    std::string why;
    if (host::Installed(&why)) return true;
    *ret = Fail(L, why);
    return false;
}

// Works in the console. The full Client ID is never returned (appHint: its last 4 characters).
int LSetup(lua_State* L) {
    if (ModRec* m = Current(); m && CurrentGen()) host::Touch(m->id);
    const host::Setup st = host::GetSetup();
    lua_createtable(L, 0, 6);
    if (!st.app.empty()) {
        lua_pushstring(L, st.app.c_str());
        lua_setfield(L, -2, "app");
        lua_pushlstring(L, st.appHint.data(), st.appHint.size());
        lua_setfield(L, -2, "appHint");
    }
    lua_pushboolean(L, st.builtin);
    lua_setfield(L, -2, "builtin");
    lua_pushboolean(L, st.ini);
    lua_setfield(L, -2, "ini");
    lua_pushlstring(L, st.redirectUri.data(), st.redirectUri.size());
    lua_setfield(L, -2, "redirectUri");
    lua_pushstring(L, logic::kDashboardUrl);
    lua_setfield(L, -2, "dashboardUrl");
    return 1;
}

// id: a string (saved after trimming ASCII whitespace; "" clears) or nil (clears). Needs a click; does not consume it.
int LSetClientId(lua_State* L) {
    ModRec* m = NeedMod(L, "setClientId");
    std::string id;
    bool tooLong = false;
    if (!lua_isnoneornil(L, 1)) {
        if (lua_type(L, 1) != LUA_TSTRING) luaL_error(L, "wum.spotify.setClientId: id must be a string or nil");
        size_t n = 0;
        const char* p = lua_tolstring(L, 1, &n);
        if (n > kMaxStr) tooLong = true;
        else id.assign(p, n);
    }
    host::Touch(m->id);
    int ret = 0;
    if (!Up(L, &ret)) return ret;
    if (!Clicked(m)) return Fail(L, "gesture");
    if (tooLong) return Invalid(L);
    const size_t a = id.find_first_not_of(" \t\r\n\f\v");
    id = a == std::string::npos ? std::string() : id.substr(a, id.find_last_not_of(" \t\r\n\f\v") - a + 1);
    std::string why;
    if (!host::SetClientId(m->id, id, &why)) return Fail(L, why);
    SysLog(1, id.empty() ? "spotify client id cleared (user click)" : "spotify client id saved (user click)", m->id);
    return True(L);
}

// The Spotify Client ID in the clipboard (not saved). Needs a click; does not consume it.
int LPasteClientId(lua_State* L) {
    ModRec* m = NeedMod(L, "pasteClientId");
    host::Touch(m->id);
    int ret = 0;
    if (!Up(L, &ret)) return ret;
    if (!Clicked(m)) return Fail(L, "gesture");
    std::string id, why;
    if (!host::ClipboardClientId(&id, &why)) return Fail(L, why);
    lua_pushlstring(L, id.data(), id.size());
    return 1;
}

// copy("redirectUri"): a fixed Melange string to the clipboard. Needs a click; does not consume it.
int LCopy(lua_State* L) {
    ModRec* m = NeedMod(L, "copy");
    luaL_checktype(L, 1, LUA_TSTRING);
    size_t n = 0;
    const char* p = lua_tolstring(L, 1, &n);
    const std::string what(p, n);
    host::Touch(m->id);
    int ret = 0;
    if (!Up(L, &ret)) return ret;
    if (what != "redirectUri") return Invalid(L);
    if (!Clicked(m)) return Fail(L, "gesture");
    std::string why;
    if (!host::CopyText(host::GetSetup().redirectUri, &why)) return Fail(L, why);
    return True(L);
}

// Opens Spotify's "create app" page (fixed). Consumes the click.
int LOpenDashboard(lua_State* L) {
    ModRec* m = NeedMod(L, "openDashboard");
    host::Touch(m->id);
    int ret = 0;
    if (!Up(L, &ret)) return ret;
    if (!Clicked(m)) return Fail(L, "gesture");
    std::string why;
    if (!host::OpenDashboard(&why)) return Fail(L, why);
    input::gesture::Clear(m->id.c_str());  // one click, one page
    SysLog(1, "spotify dashboard opened (user click)", m->id);
    return True(L);
}

// Ends an open sign-in without signing out; clears an "error" state. No click; works in the console.
int LCancel(lua_State* L) {
    std::string mod;
    if (ModRec* m = Current(); m && CurrentGen()) {
        mod = m->id;
        host::Touch(mod);
    }
    lua_pushboolean(L, host::Cancel(mod));
    return 1;
}

int LDisconnect(lua_State* L) {
    ModRec* m = NeedMod(L, "disconnect");
    host::Touch(m->id);
    host::Disconnect(m->id);
    return True(L);
}

int LLibrary(lua_State* L) {
    ModRec* m = NeedMod(L, "library");
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    std::string kind, id;
    if (StrField(L, 1, "library", "kind", &kind) == Got::Absent) luaL_error(L, "wum.spotify.library: opts.kind must be a string");
    int64_t offset = 0, limit = 50;
    const Got go = IntField(L, 1, "library", "offset", 0, kMaxOffset, &offset);
    const Got gl = IntField(L, 1, "library", "limit", 1, 50, &limit);
    const Got gi = StrField(L, 1, "library", "id", &id);
    int ret = 0;
    if (!Ready(L, m, &ret)) return ret;
    if (go == Got::Invalid || gl == Got::Invalid) return Invalid(L);
    logic::Request r;
    r.offset = static_cast<int>(offset);
    r.limit = static_cast<int>(limit);
    if (kind == "tracks") {
        r.op = logic::Op::Tracks;
    } else if (kind == "playlists") {
        r.op = logic::Op::Playlists;
    } else if (kind == "playlist") {
        if (gi != Got::Ok || !logic::ValidId(id)) return Invalid(L);
        r.op = logic::Op::PlaylistTracks;
        r.id = id;
    } else {
        return Invalid(L);
    }
    return SubmitReq(L, m, r, 2, "library");
}

int LDevices(lua_State* L) {
    ModRec* m = NeedMod(L, "devices");
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int ret = 0;
    if (!Ready(L, m, &ret)) return ret;
    logic::Request r;
    r.op = logic::Op::Devices;
    return SubmitReq(L, m, r, 1, "devices");
}

int LPlayback(lua_State* L) {
    ModRec* m = NeedMod(L, "playback");
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int ret = 0;
    if (!Ready(L, m, &ret)) return ret;
    logic::Request r;
    r.op = logic::Op::Playback;
    return SubmitReq(L, m, r, 1, "playback");
}

// opts: exactly one of uri, uris (1..50) or context (+ offset); positionMs, deviceId.
int LPlay(lua_State* L) {
    ModRec* m = NeedMod(L, "play");
    luaL_checktype(L, 1, LUA_TTABLE);
    const int fn = OptFn(L, 2);
    std::string uri, context, deviceId, offsetUri;
    const Got gu = StrField(L, 1, "play", "uri", &uri);
    const Got gc = StrField(L, 1, "play", "context", &context);
    const Got gd = StrField(L, 1, "play", "deviceId", &deviceId);
    int64_t position = -1, offsetIndex = -1;
    const Got gp = IntField(L, 1, "play", "positionMs", 0, kMaxPositionMs, &position);
    std::vector<std::string> uris;
    bool haveUris = false, bad = false;
    if (const int t = lua_getfield(L, 1, "uris"); t != LUA_TNIL) {
        if (t != LUA_TTABLE) luaL_error(L, "wum.spotify.play: opts.uris must be a table");
        haveUris = true;
        const lua_Unsigned n = lua_rawlen(L, -1);
        if (n < 1 || n > 50) {
            bad = true;
        } else {
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
                if (lua_rawgeti(L, -1, i) != LUA_TSTRING) luaL_error(L, "wum.spotify.play: opts.uris[%d] must be a string", static_cast<int>(i));
                size_t len = 0;
                const char* s = lua_tolstring(L, -1, &len);
                if (len > kMaxStr) bad = true;
                else uris.emplace_back(s, len);
                lua_pop(L, 1);
            }
        }
    }
    lua_pop(L, 1);
    bool haveOffset = false;
    if (const int t = lua_getfield(L, 1, "offset"); t != LUA_TNIL) {
        haveOffset = true;
        if (t == LUA_TNUMBER) {
            if (!IntValue(L, -1, 0, 100000, &offsetIndex)) bad = true;
        } else if (t == LUA_TSTRING) {
            size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            offsetUri.assign(s, std::min<size_t>(len, kMaxStr + 1));
            if (logic::ClassifyUri(offsetUri) != logic::UriKind::Track) bad = true;
        } else {
            luaL_error(L, "wum.spotify.play: opts.offset must be a number or a string");
        }
    }
    lua_pop(L, 1);
    int ret = 0;
    if (!Ready(L, m, &ret)) return ret;
    const int sources = (gu != Got::Absent ? 1 : 0) + (haveUris ? 1 : 0) + (gc != Got::Absent ? 1 : 0);
    if (sources != 1 || bad || gu == Got::Invalid || gc == Got::Invalid || gd == Got::Invalid || gp == Got::Invalid) return Invalid(L);
    if (haveOffset && gc == Got::Absent) return Invalid(L);
    logic::Request r;
    r.op = logic::Op::Play;
    if (gu == Got::Ok) {
        if (logic::ClassifyUri(uri) != logic::UriKind::Track) return Invalid(L);
        r.uris.push_back(uri);
    } else if (haveUris) {
        for (const std::string& u : uris)
            if (logic::ClassifyUri(u) != logic::UriKind::Track) return Invalid(L);
        r.uris = std::move(uris);
    } else {
        if (logic::ClassifyUri(context) != logic::UriKind::Playlist) return Invalid(L);
        r.context = context;
        r.offsetIndex = static_cast<int>(offsetIndex);
        if (!offsetUri.empty()) r.offsetIndex = -1;
        r.offsetUri = offsetUri;
    }
    if (gd == Got::Ok) {
        if (!logic::ValidDeviceId(deviceId)) return Invalid(L);
        r.deviceId = deviceId;
    }
    r.positionMs = gp == Got::Ok ? position : -1;
    return SubmitReq(L, m, r, fn, "play");
}

int Simple(lua_State* L, const char* name, logic::Op op) {
    ModRec* m = NeedMod(L, name);
    const int fn = OptFn(L, 1);
    int ret = 0;
    if (!Ready(L, m, &ret)) return ret;
    logic::Request r;
    r.op = op;
    return SubmitReq(L, m, r, fn, name);
}
int LPause(lua_State* L) { return Simple(L, "pause", logic::Op::Pause); }
int LResume(lua_State* L) { return Simple(L, "resume", logic::Op::Resume); }

int LSeek(lua_State* L) {
    ModRec* m = NeedMod(L, "seek");
    luaL_checktype(L, 1, LUA_TNUMBER);  // a numeric string raises too
    const int fn = OptFn(L, 2);
    int ret = 0;
    if (!Ready(L, m, &ret)) return ret;
    int64_t ms = 0;
    if (!IntValue(L, 1, 0, kMaxPositionMs, &ms)) return Invalid(L);
    logic::Request r;
    r.op = logic::Op::Seek;
    r.positionMs = ms;
    return SubmitReq(L, m, r, fn, "seek");
}

int LVolume(lua_State* L) {
    ModRec* m = NeedMod(L, "volume");
    luaL_checktype(L, 1, LUA_TNUMBER);  // a numeric string raises too
    const int fn = OptFn(L, 2);
    int ret = 0;
    if (!Ready(L, m, &ret)) return ret;
    int64_t pct = 0;
    if (!IntValue(L, 1, 0, 100, &pct)) return Invalid(L);
    logic::Request r;
    r.op = logic::Op::Volume;
    r.volume = static_cast<int>(pct);
    return SubmitReq(L, m, r, fn, "volume");
}

// opts: deviceId (required, from devices()), play (boolean, optional). A playback control call.
int LTransfer(lua_State* L) {
    ModRec* m = NeedMod(L, "transfer");
    luaL_checktype(L, 1, LUA_TTABLE);
    const int fn = OptFn(L, 2);
    std::string deviceId;
    const Got gd = StrField(L, 1, "transfer", "deviceId", &deviceId);
    bool play = false;
    if (const int t = lua_getfield(L, 1, "play"); t != LUA_TNIL) {
        if (t != LUA_TBOOLEAN) luaL_error(L, "wum.spotify.transfer: opts.play must be a boolean");
        play = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    int ret = 0;
    if (!Ready(L, m, &ret)) return ret;
    if (gd != Got::Ok || !logic::ValidDeviceId(deviceId)) return Invalid(L);
    logic::Request r;
    r.op = logic::Op::Transfer;
    r.deviceId = deviceId;
    r.play = play;
    return SubmitReq(L, m, r, fn, "transfer");
}

void Shared(lua_State* L, int wum) {
    host::SetSink(&g_sink);  // no per-VM state here: requests live in the host, callbacks in the generations
    static const luaL_Reg kSpotify[] = {{"available", LAvailable},
                                        {"status", LStatus},
                                        {"connect", LConnect},
                                        {"disconnect", LDisconnect},
                                        {"library", LLibrary},
                                        {"devices", LDevices},
                                        {"play", LPlay},
                                        {"pause", LPause},
                                        {"resume", LResume},
                                        {"seek", LSeek},
                                        {"volume", LVolume},
                                        {"playback", LPlayback},
                                        {"transfer", LTransfer},
                                        // Melange 0.14: the in-game setup
                                        {"setup", LSetup},
                                        {"setClientId", LSetClientId},
                                        {"pasteClientId", LPasteClientId},
                                        {"copy", LCopy},
                                        {"openDashboard", LOpenDashboard},
                                        {"cancel", LCancel},
                                        {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kSpotify);
    lua_setfield(L, wum, "spotify");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
