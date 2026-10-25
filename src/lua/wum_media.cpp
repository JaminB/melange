// wum.media: watch-party playback for client mods (the media pack, players, resolve, torrents). Presentation only, in
// the client Sandbox VM. The work is done by src/media/* (host.h, pack.h); this file scopes players and torrents to the
// calling mod generation: a handle is a full userdata (wum_media_handles.h) that only its own generation can use, and
// every handle is closed by the generation's cleanup (unload, reload, disable).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "input/gesture.h"
#include "lua/event_names.h"
#include "lua/sandbox_core.h"
#include "lua/wum_media_handles.h"
#include "media/host.h"
#include "media/pack.h"
#include "media/url_logic.h"

namespace melange::sandbox {
namespace {
namespace url = melange::media::url;
namespace host = melange::media::host;
namespace pack = melange::media::pack;
using mediaud::PlayerUd;
using mediaud::TorrentUd;

constexpr uint32_t kGestureMs = 2000;  // wum.media.install: the click must be this recent

int Fail(lua_State* L, const char* why) {
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}
int Fail(lua_State* L, const std::string& why) { return Fail(L, why.c_str()); }
int Closed(lua_State* L) { return Fail(L, "closed"); }
int True(lua_State* L) {
    lua_pushboolean(L, true);
    return 1;
}

void SetStr(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}
void SetNum(lua_State* L, const char* k, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}
void SetInt(lua_State* L, const char* k, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}
void SetBool(lua_State* L, const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
}

ModRec* NeedMod(lua_State* L, const char* fn) {
    ModRec* m = Current();
    if (!m || !CurrentGen()) luaL_error(L, "wum.media.%s needs a mod context (not available in the console environment)", fn);
    return m;
}

// ---------------------------------------------------------------- delivery from the media host
class LuaSink final : public host::Sink {
public:
    void Event(const std::string& mod, const json::Value& payload) override { QueueEventFor(mod, ev::kMedia, payload); }
    void Callback(const std::string& mod, uint32_t token, const json::Value& arg, bool done) override {
        sandbox::Callback* cb = FindCallback(token);
        if (CallbackLive(cb) && cb->kind == CbKind::Media && cb->gen->mod->id == mod)
            Invoke(cb, [&arg](lua_State* L) {
                PushJson(L, arg);
                return 1;
            });
        if (done && cb) KillCallback(token);
    }
};
LuaSink g_sink;

// ---------------------------------------------------------------- handles
// The handle at arg 1 if the calling generation made it and it is open; `mod` gets the owner. A wrong type raises.
template <class Ud>
Ud* Handle(lua_State* L, const char* meta, std::string* mod) {
    auto* ud = static_cast<Ud*>(luaL_checkudata(L, 1, meta));
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g || ud->id == 0 || ud->serial != g->serial) return nullptr;
    *mod = m->id;
    return ud;
}
PlayerUd* PlayerArg(lua_State* L, std::string* mod) { return Handle<PlayerUd>(L, mediaud::kPlayerMeta, mod); }
TorrentUd* TorrentArg(lua_State* L, std::string* mod) { return Handle<TorrentUd>(L, mediaud::kTorrentMeta, mod); }

// A host call that said "not mine / not open": the handle is dead for good.
template <class Ud>
int Dead(lua_State* L, Ud* ud) {
    ud->id = 0;
    return Closed(L);
}

#define MEDIA_PLAYER_OR_CLOSED(ud, mod) \
    std::string mod;                    \
    PlayerUd* ud = PlayerArg(L, &mod);  \
    if (!ud) return Closed(L)

int PPlay(lua_State* L) {
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    return host::SetPaused(mod, ud->id, false) ? True(L) : Dead(L, ud);
}
int PPause(lua_State* L) {
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    return host::SetPaused(mod, ud->id, true) ? True(L) : Dead(L, ud);
}
int PSeek(lua_State* L) {
    const double t = luaL_checknumber(L, 2);
    const char* modes[] = {"exact", "keyframes", "relative", nullptr};
    const int mode = luaL_checkoption(L, 3, "exact", modes);
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    if (!std::isfinite(t)) return Fail(L, "invalid");
    return host::Seek(mod, ud->id, t, modes[mode]) ? True(L) : Dead(L, ud);
}
int PSpeed(lua_State* L) {
    const double x = luaL_checknumber(L, 2);
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    if (!std::isfinite(x)) return Fail(L, "invalid");
    return host::SetSpeed(mod, ud->id, x) ? True(L) : Dead(L, ud);
}
int PVolume(lua_State* L) {
    const double v = luaL_checknumber(L, 2);
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    if (!std::isfinite(v)) return Fail(L, "invalid");
    return host::SetVolume(mod, ud->id, v) ? True(L) : Dead(L, ud);
}
int PMute(lua_State* L) {
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    const bool b = lua_toboolean(L, 2);
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    return host::SetMute(mod, ud->id, b) ? True(L) : Dead(L, ud);
}
int PTrack(lua_State* L) {
    const char* kinds[] = {"audio", "sub", nullptr};
    const int kind = luaL_checkoption(L, 2, nullptr, kinds);
    int id = 0;
    if (lua_type(L, 3) == LUA_TBOOLEAN && !lua_toboolean(L, 3)) id = 0;
    else id = static_cast<int>(luaL_checkinteger(L, 3));
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    if (id < 0 || id > 9999 || (id == 0 && lua_type(L, 3) != LUA_TBOOLEAN)) return Fail(L, "invalid");
    return host::SetTrack(mod, ud->id, kind == 0, id) ? True(L) : Dead(L, ud);
}
int PQuality(lua_State* L) {
    const lua_Integer h = luaL_checkinteger(L, 2);
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    return host::SetQuality(mod, ud->id, static_cast<int>(std::clamp<lua_Integer>(h, 0, 100000))) ? True(L) : Dead(L, ud);
}
int PSize(lua_State* L) {
    const double w = luaL_checknumber(L, 2), h = luaL_checknumber(L, 3);
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    if (!std::isfinite(w) || !std::isfinite(h)) return Fail(L, "invalid");
    return host::SetSize(mod, ud->id, static_cast<int>(std::clamp(w, 0.0, 100000.0)), static_cast<int>(std::clamp(h, 0.0, 100000.0)))
               ? True(L)
               : Dead(L, ud);
}

// st = {e, i, s, p, r, a} (design 1.1.3); hostId the host's SteamID as a string.
int PFollow(lua_State* L) {
    const bool stop = lua_isnoneornil(L, 2);
    if (!stop) {
        luaL_checktype(L, 2, LUA_TTABLE);
        luaL_checkstring(L, 3);
    }
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    if (stop) return host::Follow(mod, ud->id, nullptr, 0) ? True(L) : Dead(L, ud);
    host::FollowState st;
    auto num = [&](const char* k, double* out) {
        const bool ok = lua_getfield(L, 2, k) == LUA_TNUMBER && std::isfinite(lua_tonumber(L, -1));
        if (ok) *out = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return ok;
    };
    bool ok = num("e", &st.e) && num("p", &st.p) && num("r", &st.r) && num("a", &st.a);
    if (lua_getfield(L, 2, "s") == LUA_TSTRING) {
        const std::string s = lua_tostring(L, -1);
        ok = ok && (s == "play" || s == "pause");
        st.play = s == "play";
    } else {
        ok = false;
    }
    lua_pop(L, 1);
    const int it = lua_getfield(L, 2, "i");
    if (it == LUA_TSTRING || it == LUA_TNUMBER) {
        size_t n = 0;
        const char* s = lua_tolstring(L, -1, &n);
        st.i.assign(s, std::min<size_t>(n, 128));
    }
    lua_pop(L, 1);
    const char* hs = lua_tostring(L, 3);
    char* end = nullptr;
    const unsigned long long hostId = std::strtoull(hs, &end, 10);
    if (!ok || !end || *end || hostId == 0 || !(st.r > 0) || st.p < 0) return Fail(L, "invalid");
    return host::Follow(mod, ud->id, &st, hostId) ? True(L) : Dead(L, ud);
}

void PushTracks(lua_State* L, const std::vector<host::Track>& v) {
    lua_createtable(L, static_cast<int>(v.size()), 0);
    for (size_t i = 0; i < v.size(); ++i) {
        lua_createtable(L, 0, 4);
        SetInt(L, "id", v[i].id);
        SetStr(L, "lang", v[i].lang);
        SetStr(L, "title", v[i].title);
        SetBool(L, "selected", v[i].selected);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}

int PState(lua_State* L) {
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    host::PlayerState s;
    if (!host::State(mod, ud->id, &s)) return Dead(L, ud);
    lua_createtable(L, 0, 18);
    SetNum(L, "time", s.time);
    SetNum(L, "duration", s.duration);
    SetBool(L, "paused", s.paused);
    SetBool(L, "buffering", s.buffering);
    SetNum(L, "cacheAhead", s.cacheAhead);
    SetNum(L, "speed", s.speed);
    SetStr(L, "title", s.title);
    SetBool(L, "eof", s.eof);
    SetBool(L, "live", s.live);
    SetInt(L, "w", s.w);
    SetInt(L, "h", s.h);
    SetNum(L, "aspect", s.aspect);
    lua_createtable(L, 0, 2);
    PushTracks(L, s.audio);
    lua_setfield(L, -2, "audio");
    PushTracks(L, s.sub);
    lua_setfield(L, -2, "sub");
    lua_setfield(L, -2, "tracks");
    SetBool(L, "following", s.following);
    if (s.hasDrift) SetNum(L, "drift", s.drift);
    SetStr(L, "syncAction", s.syncAction);
    SetStr(L, "helper", s.helper);
    return 1;
}

int PClose(lua_State* L) {
    MEDIA_PLAYER_OR_CLOSED(ud, mod);
    host::Close(mod, ud->id);
    ud->id = 0;
    return True(L);
}

void PushFiles(lua_State* L, const std::vector<host::File>& files) {
    lua_createtable(L, static_cast<int>(files.size()), 0);
    for (size_t i = 0; i < files.size(); ++i) {
        lua_createtable(L, 0, 4);
        SetInt(L, "index", files[i].index);
        SetStr(L, "path", files[i].path);
        SetNum(L, "size", files[i].size);
        SetBool(L, "video", files[i].video);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}

int TFiles(lua_State* L) {
    std::string mod;
    TorrentUd* ud = TorrentArg(L, &mod);
    if (!ud) return Closed(L);
    std::vector<host::File> files;
    bool have = false;
    if (!host::TorrentFiles(mod, ud->id, &files, &have)) return Dead(L, ud);
    if (!have) {
        lua_pushnil(L);
        return 1;
    }
    PushFiles(L, files);
    return 1;
}

int TStatus(lua_State* L) {
    std::string mod;
    TorrentUd* ud = TorrentArg(L, &mod);
    if (!ud) return Closed(L);
    host::TorrentStatus s;
    if (!host::TorrentStatusOf(mod, ud->id, &s)) return Dead(L, ud);
    lua_createtable(L, 0, 6);
    SetNum(L, "down", s.down);
    SetNum(L, "up", s.up);
    SetInt(L, "peers", s.peers);
    SetNum(L, "progress", s.progress);
    SetNum(L, "have", s.have);
    SetBool(L, "metadata", s.metadata);
    return 1;
}

int TClose(lua_State* L) {
    std::string mod;
    TorrentUd* ud = TorrentArg(L, &mod);
    if (!ud) return Closed(L);
    host::CloseTorrent(mod, ud->id);
    ud->id = 0;
    return True(L);
}

int HandleToString(lua_State* L) {
    lua_pushstring(L, luaL_testudata(L, 1, mediaud::kPlayerMeta) ? "media player" : "media torrent");
    return 1;
}

void MakeMeta(lua_State* L, const char* name, const luaL_Reg* methods) {
    luaL_newmetatable(L, name);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, "locked");
    lua_setfield(L, -2, "__metatable");  // getmetatable() sees a string; setmetatable cannot replace it
    lua_pushcfunction(L, &HandleToString);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

// ---------------------------------------------------------------- wum.media.*
int Available(lua_State* L) {
    std::string why;
    if (host::Available(&why)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, false);
    lua_pushstring(L, why.c_str());
    return 2;
}

int Pack(lua_State* L) {
    if (ModRec* m = Current()) host::Touch(m->id);
    const pack::Info i = pack::Get();
    lua_createtable(L, 0, 10);
    SetStr(L, "state", pack::StateName(i.state));
    SetStr(L, "version", i.version);
    SetNum(L, "progress", i.progress);
    SetNum(L, "bytes", static_cast<double>(i.bytes));
    SetNum(L, "total", static_cast<double>(i.total));
    SetInt(L, "downloadMB", i.downloadMB);
    SetInt(L, "diskMB", i.diskMB);
    SetStr(L, "ytdlp", i.ytdlp);
    if (i.state == pack::State::Error) SetStr(L, "error", i.error.empty() ? "network" : i.error);
    return 1;
}

// The pack download: a consent-grade click on one of this mod's regions in the last 2 s is the user's consent (S10).
int Install(lua_State* L) {
    ModRec* m = NeedMod(L, "install");
    host::Touch(m->id);
    std::string why;
    if (!host::Available(&why)) return Fail(L, why == "unavailable" ? "disabled" : why);
    if (pack::Get().state == pack::State::Ready) return Fail(L, "ready");
    if (!input::gesture::Recent(m->id.c_str(), kGestureMs)) return Fail(L, "gesture");
    if (!pack::Install(m->id.c_str(), &why)) return Fail(L, why);
    input::gesture::Clear(m->id.c_str());  // one click, one install
    SysLog(1, "media pack install requested (user click)", m->id);
    return True(L);
}

int CancelInstall(lua_State* L) {
    pack::CancelInstall();
    return True(L);
}

int Classify(lua_State* L) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    url::Classified c;
    if (!url::Classify(std::string_view(s, n), &c)) return Fail(L, "unsupported-url");
    lua_createtable(L, 0, 4);
    SetStr(L, "url", c.url);
    if (c.kind == url::Kind::YouTube) {
        SetStr(L, "kind", "youtube");
        if (!c.yt.id.empty()) SetStr(L, "id", c.yt.id);
        if (!c.yt.playlist.empty()) SetStr(L, "playlist", c.yt.playlist);
        if (c.yt.live) SetBool(L, "live", true);
    } else {
        SetStr(L, "kind", "magnet");
        SetStr(L, "infohash", c.bt.infohash);
        if (!c.bt.name.empty()) SetStr(L, "name", c.bt.name);
    }
    return 1;
}

int Resolve(lua_State* L) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    ModRec* m = NeedMod(L, "resolve");
    url::Classified c;
    if (!url::Classify(std::string_view(s, n), &c) || c.kind != url::Kind::YouTube) return Fail(L, "unsupported-url");
    Callback* cb = NewCallback(L, 2, CbKind::Media, "resolve");
    Activate(cb);
    std::string why;
    const uint32_t rid = host::Resolve(m->id, c.url, cb->id, &why);
    if (!rid) {
        KillCallback(cb->id);
        return Fail(L, why);
    }
    const std::string mod = m->id;
    CurrentGen()->cleanups.push_back([mod, rid] { host::CancelResolve(mod, rid); });
    lua_pushinteger(L, rid);
    return 1;
}

int Torrent(lua_State* L) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    const bool hasFn = !lua_isnoneornil(L, 2);
    if (hasFn) luaL_checktype(L, 2, LUA_TFUNCTION);
    ModRec* m = NeedMod(L, "torrent");
    Gen* g = CurrentGen();
    url::Classified c;
    if (!url::Classify(std::string_view(s, n), &c) || c.kind != url::Kind::Magnet) return Fail(L, "unsupported-url");
    Callback* cb = nullptr;
    if (hasFn) {
        cb = NewCallback(L, 2, CbKind::Media, "torrent");
        Activate(cb);
    }
    std::string why;
    const uint32_t id = host::AddTorrent(m->id, c.url, cb ? cb->id : 0, Game().inMatch, &why);
    if (!id) {
        if (cb) KillCallback(cb->id);
        return Fail(L, why);
    }
    const std::string mod = m->id;
    g->cleanups.push_back([mod, id] { host::CloseTorrent(mod, id); });
    auto* ud = static_cast<TorrentUd*>(lua_newuserdatauv(L, sizeof(TorrentUd), 0));
    ud->id = id;
    ud->serial = g->serial;
    luaL_setmetatable(L, mediaud::kTorrentMeta);
    return 1;
}

// opts field helpers: absent or nil gives the default; a wrong type raises.
double OptNum(lua_State* L, const char* k, double def) {
    const int t = lua_getfield(L, 1, k);
    double v = def;
    if (t != LUA_TNIL) {
        if (t != LUA_TNUMBER) luaL_error(L, "wum.media.open: opts.%s must be a number", k);
        v = lua_tonumber(L, -1);
        if (!std::isfinite(v)) v = def;
    }
    lua_pop(L, 1);
    return v;
}
bool OptBool(lua_State* L, const char* k, bool def) {
    const int t = lua_getfield(L, 1, k);
    bool v = def;
    if (t != LUA_TNIL) {
        if (t != LUA_TBOOLEAN) luaL_error(L, "wum.media.open: opts.%s must be a boolean", k);
        v = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return v;
}
std::string OptStr(lua_State* L, const char* k) {
    const int t = lua_getfield(L, 1, k);
    std::string v;
    if (t != LUA_TNIL) {
        if (t != LUA_TSTRING) luaL_error(L, "wum.media.open: opts.%s must be a string", k);
        size_t n = 0;
        const char* s = lua_tolstring(L, -1, &n);
        v.assign(s, std::min<size_t>(n, 8192));
    }
    lua_pop(L, 1);
    return v;
}

int Open(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ModRec* m = NeedMod(L, "open");
    Gen* g = CurrentGen();
    host::OpenSpec spec;
    const std::string link = OptStr(L, "url");
    const int tt = lua_getfield(L, 1, "torrent");
    TorrentUd* tud = nullptr;
    if (tt != LUA_TNIL) {
        tud = mediaud::TestTorrent(L, -1);
        if (!tud) luaL_error(L, "wum.media.open: opts.torrent must be a torrent from wum.media.torrent");
    }
    lua_pop(L, 1);
    const double file = OptNum(L, "file", -1);
    spec.start = std::max(0.0, OptNum(L, "start", 0));
    spec.paused = OptBool(L, "paused", true);
    spec.volume = static_cast<int>(std::lround(std::clamp(OptNum(L, "volume", 70), 0.0, 100.0)));
    spec.mute = OptBool(L, "mute", false);
    spec.quality = static_cast<int>(std::clamp(OptNum(L, "quality", 720), 0.0, 100000.0));
    spec.speed = std::clamp(OptNum(L, "speed", 1), 0.25, 4.0);
    spec.alang = OptStr(L, "audioLang");
    spec.slang = OptStr(L, "subLang");
    if (!url::ValidLang(spec.alang)) spec.alang.clear();  // a bad code is dropped, never passed on (S1)
    if (!url::ValidLang(spec.slang)) spec.slang.clear();
    if (link.empty() == (tud == nullptr)) return Fail(L, "unsupported-url");  // exactly one source
    if (!link.empty()) {
        url::Classified c;
        if (!url::Classify(link, &c) || c.kind != url::Kind::YouTube || c.yt.id.empty()) return Fail(L, "unsupported-url");
        spec.url = url::CanonicalVideoUrl(c.yt.id);
        spec.live = c.yt.live;
    } else {
        if (tud->id == 0 || tud->serial != g->serial) return Closed(L);
        if (file < 0 || file != std::floor(file)) return Fail(L, "unsupported-url");
        spec.torrent = tud->id;
        spec.file = static_cast<int>(file);
    }
    std::string why;
    const uint32_t id = host::Open(m->id, spec, &why);
    if (!id) return Fail(L, why);
    const std::string mod = m->id;
    g->cleanups.push_back([mod, id] { host::Close(mod, id); });
    auto* ud = static_cast<PlayerUd*>(lua_newuserdatauv(L, sizeof(PlayerUd), 0));
    ud->id = id;
    ud->serial = g->serial;
    luaL_setmetatable(L, mediaud::kPlayerMeta);
    return 1;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kPlayer[] = {{"play", PPlay},       {"pause", PPause},     {"seek", PSeek},   {"speed", PSpeed},
                                       {"volume", PVolume},   {"mute", PMute},       {"track", PTrack}, {"quality", PQuality},
                                       {"size", PSize},       {"follow", PFollow},   {"state", PState}, {"close", PClose},
                                       {nullptr, nullptr}};
    static const luaL_Reg kTorrent[] = {{"files", TFiles}, {"status", TStatus}, {"close", TClose}, {nullptr, nullptr}};
    MakeMeta(L, mediaud::kPlayerMeta, kPlayer);
    MakeMeta(L, mediaud::kTorrentMeta, kTorrent);
    host::SetSink(&g_sink);
    static const luaL_Reg kMedia[] = {{"available", Available}, {"pack", Pack},       {"install", Install},
                                      {"cancelInstall", CancelInstall},                {"classify", Classify},
                                      {"resolve", Resolve},     {"torrent", Torrent}, {"open", Open},
                                      {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kMedia);
    lua_setfield(L, wum, "media");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
