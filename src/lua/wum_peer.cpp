// wum.lobby and wum.peer: the Steam lobby as client mods see it, and per-mod message channels on the peer bus
// (Steam P2P channel 6). Presentation only, in the client Sandbox VM; nothing here touches the sim, channel 0 or
// Wormsign. The work is done by src/net/peerbus.cpp (peerbus_api.h); this file scopes channels to the calling mod
// generation and converts Lua values to and from the bus's tagged binary (peerbus_wire.h).
#include <cerrno>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "lua/event_names.h"
#include "lua/sandbox_core.h"
#include "net/peerbus.h"
#include "net/peerbus_api.h"

namespace melange::sandbox {
namespace {
namespace wire = peerbus::wire;
constexpr const char kChannelMeta[] = "melange.peer.channel";

// A channel as Lua holds it. The mod's channel survives a reload of the same version (the new generation takes it
// over, so peers never see this machine go away); a userdata of an older generation then reads as closed.
struct ChUd {
    uint32_t bus;
    uint32_t serial;
};
struct Chan {
    peerbus::ChannelId bus = 0;
    int version = 0;
    ModRec* mod = nullptr;
    uint32_t serial = 0;      // the owning generation
    uint32_t prevSerial = 0;  // the generation it was taken over from, while that reload is in flight
    std::vector<uint32_t> on, onPeer;
    std::vector<uint32_t> prevOn, prevOnPeer;  // the taken-over generation's, given back if the reload fails
};
std::map<std::string, Chan> g_chans;  // by mod id

ModRec* NeedMod(lua_State* L, const char* fn) {
    ModRec* m = Current();
    if (!m || !CurrentGen()) luaL_error(L, "%s needs a mod context (not available in the console environment)", fn);
    return m;
}

std::string IdText(uint64_t id) { return std::to_string(id); }

uint64_t ParseId(const char* s) {
    if (!s || !*s) return 0;
    for (const char* p = s; *p; ++p)
        if (*p < '0' || *p > '9') return 0;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = strtoull(s, &end, 10);
    return errno || *end ? 0 : static_cast<uint64_t>(v);
}

Chan* ChanByBus(uint32_t bus) {
    for (auto& [id, c] : g_chans)
        if (c.bus == bus) return &c;
    return nullptr;
}

// The live channel behind the userdata at 1, or nullptr when it was closed (or belongs to an older generation).
Chan* Live(lua_State* L) {
    auto* ud = static_cast<ChUd*>(luaL_checkudata(L, 1, kChannelMeta));
    Chan* c = ChanByBus(ud->bus);
    return c && c->serial == ud->serial ? c : nullptr;
}

int Fail(lua_State* L, const char* why) {
    lua_pushboolean(L, false);
    lua_pushstring(L, why);
    return 2;
}

int NilFail(lua_State* L, const char* why) {
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

void KillAll(std::vector<uint32_t>& ids) {
    for (uint32_t id : ids) KillCallback(id);
    ids.clear();
}

// ---------------------------------------------------------------- Lua <-> wire::Value
// A running lower bound of the encoded size, so a huge (or self-referencing) table is refused before it is walked.
struct Budget {
    size_t bytes = 0;
};

bool ToValue(lua_State* L, int idx, int depth, wire::Value* out, Budget* b) {
    idx = lua_absindex(L, idx);
    if (++b->bytes > wire::kMaxReliable) return false;
    switch (lua_type(L, idx)) {
        case LUA_TNIL: out->type = wire::Value::Type::Nil; return true;
        case LUA_TBOOLEAN:
            out->type = wire::Value::Type::Bool;
            out->b = lua_toboolean(L, idx) != 0;
            return true;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                out->type = wire::Value::Type::Int;
                out->i = static_cast<int64_t>(lua_tointeger(L, idx));
            } else {
                out->type = wire::Value::Type::Num;
                out->n = static_cast<double>(lua_tonumber(L, idx));
                b->bytes += 8;
            }
            return true;
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, idx, &n);
            b->bytes += n;
            if (n > wire::kMaxString || b->bytes > wire::kMaxReliable) return false;
            out->type = wire::Value::Type::Str;
            out->s.assign(s, n);
            return true;
        }
        case LUA_TTABLE: {
            if (depth >= wire::kMaxDepth) return false;
            luaL_checkstack(L, 4, "wum.peer");
            out->type = wire::Value::Type::Table;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                const int kt = lua_type(L, -2);
                if (kt != LUA_TSTRING && kt != LUA_TNUMBER && kt != LUA_TBOOLEAN)
                    luaL_error(L, "wum.peer: table keys must be strings, numbers or booleans (got %s)", lua_typename(L, kt));
                wire::Value k, v;
                if (out->t.size() >= wire::kMaxKeys || !ToValue(L, -2, depth, &k, b) || !ToValue(L, -1, depth + 1, &v, b)) {
                    lua_pop(L, 2);
                    return false;
                }
                out->t.emplace_back(std::move(k), std::move(v));
                lua_pop(L, 1);
            }
            return true;
        }
        default:
            luaL_error(L, "wum.peer: cannot send a %s (nil, booleans, numbers, strings and tables only)", luaL_typename(L, idx));
            return false;
    }
}

void PushValue(lua_State* L, const wire::Value& v) {
    luaL_checkstack(L, 4, "wum.peer");
    switch (v.type) {
        case wire::Value::Type::Nil: lua_pushnil(L); return;
        case wire::Value::Type::Bool: lua_pushboolean(L, v.b); return;
        case wire::Value::Type::Int: lua_pushinteger(L, static_cast<lua_Integer>(v.i)); return;
        case wire::Value::Type::Num: lua_pushnumber(L, v.n); return;
        case wire::Value::Type::Str: lua_pushlstring(L, v.s.data(), v.s.size()); return;
        case wire::Value::Type::Table:
            lua_createtable(L, 0, static_cast<int>(v.t.size()));
            for (const auto& [k, x] : v.t) {
                if (x.type == wire::Value::Type::Nil) continue;
                PushValue(L, k);  // never nil or NaN: DecodeValue refuses those keys
                PushValue(L, x);
                lua_rawset(L, -3);
            }
            return;
    }
    lua_pushnil(L);
}

// Encodes the value at idx. False when it breaks a limit ("size"); raises for a value that cannot be sent at all.
bool Encode(lua_State* L, int idx, std::vector<uint8_t>* out) {
    wire::Value v;
    Budget b;
    return ToValue(L, idx, 0, &v, &b) && wire::EncodeValue(v, out, nullptr);
}

bool ReliableOpt(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return true;
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "reliable");
    const bool r = lua_isnil(L, -1) ? true : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return r;
}

// ---------------------------------------------------------------- wum.lobby
void SetStr(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}
void SetBool(lua_State* L, const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
}

bool IsHost(const peerbus::LobbyView& v) { return v.lobby && v.owner && v.owner == v.me; }

int LobbyState(lua_State* L) {
    peerbus::LobbyView v;
    if (!peerbus::Lobby(&v)) {
        lua_pushnil(L);
        return 1;
    }
    ModRec* m = Current();
    int version = -1;  // with the caller's channel open, `peer` means that channel's version
    if (m) {
        auto it = g_chans.find(m->id);
        if (it != g_chans.end()) version = it->second.version;
    }
    lua_createtable(L, 0, 6);
    if (v.lobby) {
        SetStr(L, "lobby", IdText(v.lobby));
        SetStr(L, "me", IdText(v.me));
        if (v.owner) SetStr(L, "owner", IdText(v.owner));
    }
    SetBool(L, "isHost", IsHost(v));
    SetStr(L, "phase", v.phase);
    lua_createtable(L, static_cast<int>(v.members.size()), 0);
    int i = 0;
    for (const peerbus::LobbyMember& mem : v.members) {
        lua_createtable(L, 0, 6);
        SetStr(L, "id", IdText(mem.id));
        SetStr(L, "name", mem.name);
        SetBool(L, "me", mem.me);
        SetBool(L, "host", mem.host);
        if (!mem.melange.empty()) SetStr(L, "melange", mem.melange);
        SetBool(L, "peer", m && !mem.me && wire::Advertises(mem.advert, m->id, version));
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "members");
    return 1;
}

int LobbyIsHost(lua_State* L) {
    peerbus::LobbyView v;
    lua_pushboolean(L, peerbus::Lobby(&v) && IsHost(v));
    return 1;
}

// ---------------------------------------------------------------- wum.peer
int PeerOpen(lua_State* L) {
    ModRec* m = NeedMod(L, "wum.peer.open");
    Gen* g = CurrentGen();
    lua_Integer version = 1;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_getfield(L, 1, "version");
        if (!lua_isnil(L, -1)) {
            if (!lua_isinteger(L, -1)) return luaL_error(L, "wum.peer.open: version must be an integer");
            version = lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }
    if (version < 1 || version > 65535) return luaL_error(L, "wum.peer.open: version must be 1..65535");
    if (!peerbus::Available()) return NilFail(L, "unavailable");
    auto it = g_chans.find(m->id);
    if (it != g_chans.end() && it->second.serial == g->serial) return NilFail(L, "already-open");
    if (it != g_chans.end() && it->second.version == version) {
        // A reload: take the channel over. The old generation's callbacks die with it, unless the reload fails.
        it->second.prevSerial = it->second.serial;
        it->second.serial = g->serial;
        it->second.prevOn = std::move(it->second.on);
        it->second.prevOnPeer = std::move(it->second.onPeer);
        it->second.on.clear();
        it->second.onPeer.clear();
    } else {
        if (it != g_chans.end()) {  // a reload with another version: the old channel goes, peers see it leave
            peerbus::Close(it->second.bus);
            g_chans.erase(it);
        }
        std::string why;
        const peerbus::ChannelId bus = peerbus::Open(m->id, static_cast<int>(version), &why);
        if (!bus) return NilFail(L, why.empty() ? "unavailable" : why.c_str());
        Chan c;
        c.bus = bus;
        c.version = static_cast<int>(version);
        c.mod = m;
        c.serial = g->serial;
        it = g_chans.emplace(m->id, std::move(c)).first;
    }
    const uint32_t bus = it->second.bus, serial = g->serial;
    const std::string modId = m->id;
    g->cleanups.push_back([modId, bus, serial] {
        auto c = g_chans.find(modId);
        if (c == g_chans.end() || c->second.bus != bus || c->second.serial != serial) return;
        ModRec* mod = c->second.mod;
        if (c->second.prevSerial && mod->gen && mod->gen->serial == c->second.prevSerial) {
            c->second.serial = c->second.prevSerial;  // the reload failed: the running generation keeps its channel
            c->second.prevSerial = 0;
            c->second.on = std::move(c->second.prevOn);  // and its ch:on / ch:onPeer callbacks
            c->second.onPeer = std::move(c->second.prevOnPeer);
            c->second.prevOn.clear();
            c->second.prevOnPeer.clear();
            return;
        }
        peerbus::Close(bus);
        g_chans.erase(c);
    });
    auto* ud = static_cast<ChUd*>(lua_newuserdatauv(L, sizeof(ChUd), 0));
    ud->bus = bus;
    ud->serial = serial;
    luaL_setmetatable(L, kChannelMeta);
    return 1;
}

int PeerNow(lua_State* L) {
    lua_pushnumber(L, peerbus::NowMs());
    return 1;
}

// ---------------------------------------------------------------- channel methods
int ChSend(lua_State* L) {
    Chan* c = Live(L);
    const char* id = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    const bool reliable = ReliableOpt(L, 4);
    if (!c) return Fail(L, "closed");
    std::vector<uint8_t> bytes;
    if (!Encode(L, 3, &bytes)) return Fail(L, "size");
    const uint64_t to = ParseId(id);
    const peerbus::SendStatus s = to ? peerbus::Send(c->bus, to, bytes, reliable) : peerbus::SendStatus::NoPeer;
    if (s != peerbus::SendStatus::Ok) return Fail(L, wire::StatusName(s));
    lua_pushboolean(L, true);
    return 1;
}

int ChBroadcast(lua_State* L) {
    Chan* c = Live(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    const bool reliable = ReliableOpt(L, 3);
    std::vector<uint8_t> bytes;
    const char* why = !c ? "closed" : !Encode(L, 2, &bytes) ? "size" : nullptr;
    if (why) {
        lua_pushinteger(L, 0);
        lua_pushstring(L, why);
        return 2;
    }
    lua_pushinteger(L, peerbus::Broadcast(c->bus, bytes, reliable));
    return 1;
}

int AddCb(lua_State* L, bool peer) {
    Chan* c = Live(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!c) return NilFail(L, "closed");
    Gen* g = CurrentGen();
    if (!g || g->serial != c->serial) return luaL_error(L, "wum.peer: a channel belongs to the mod that opened it");
    Callback* cb = NewCallback(L, 2, CbKind::Peer, peer ? "peer.onPeer" : "peer.on");
    (peer ? c->onPeer : c->on).push_back(cb->id);
    Activate(cb);
    lua_pushinteger(L, cb->id);
    return 1;
}
int ChOn(lua_State* L) { return AddCb(L, false); }
int ChOnPeer(lua_State* L) { return AddCb(L, true); }

int ChPeers(lua_State* L) {
    Chan* c = Live(L);
    lua_newtable(L);
    if (!c) return 1;
    int i = 0;
    for (uint64_t id : peerbus::Peers(c->bus)) {
        const std::string s = IdText(id);
        lua_pushlstring(L, s.data(), s.size());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int ChClock(lua_State* L) {
    Chan* c = Live(L);
    const uint64_t id = ParseId(luaL_checkstring(L, 2));
    peerbus::ClockInfo ci;
    if (!c || !id || !peerbus::Clock(id, &ci)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, ci.offsetMs);
    lua_setfield(L, -2, "offset");
    lua_pushnumber(L, ci.rttMs);
    lua_setfield(L, -2, "rtt");
    lua_pushinteger(L, ci.samples);
    lua_setfield(L, -2, "samples");
    return 1;
}

int ChSetPresence(lua_State* L) {
    Chan* c = Live(L);
    std::string text;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        json::Value v;
        std::string err;
        if (!ToJson(L, 2, &v, &err)) return luaL_error(L, "ch:setPresence: %s", err.c_str());
        text = WriteJson(v);
    }
    if (!c) return Fail(L, "closed");
    if (text.size() > wire::kMaxPresence) return Fail(L, "size");
    lua_pushboolean(L, peerbus::SetPresence(c->bus, text));
    return 1;
}

int ChPresence(lua_State* L) {
    Chan* c = Live(L);
    const uint64_t id = ParseId(luaL_checkstring(L, 2));
    const std::string text = c && id ? peerbus::Presence(c->bus, id) : std::string();
    json::Value v;
    json::Error e;
    // Another machine wrote it: parse it with a cap, and only an object counts.
    if (text.empty() || !json::Parse(text, &v, &e, 2 * wire::kMaxPresence) || !v.IsObject()) {
        lua_pushnil(L);
        return 1;
    }
    PushJson(L, v);
    return 1;
}

int ChClose(lua_State* L) {
    Chan* c = Live(L);
    if (!c) {
        lua_pushboolean(L, false);
        return 1;
    }
    KillAll(c->on);
    KillAll(c->onPeer);
    peerbus::Close(c->bus);
    for (auto it = g_chans.begin(); it != g_chans.end(); ++it)
        if (&it->second == c) {
            g_chans.erase(it);
            break;
        }
    lua_pushboolean(L, true);
    return 1;
}

int ChToString(lua_State* L) {
    lua_pushstring(L, Live(L) ? "peer channel (open)" : "peer channel (closed)");
    return 1;
}

// ---------------------------------------------------------------- delivery from the bus (main thread)
void Prune(std::vector<uint32_t>& ids) {
    std::vector<uint32_t> keep;
    for (uint32_t id : ids)
        if (CallbackLive(FindCallback(id))) keep.push_back(id);
    ids.swap(keep);
}

void OnMessage(const peerbus::Delivery& d) {
    if (!Running()) return;
    Chan* c = ChanByBus(d.ch);
    if (!c) return;
    Prune(c->on);
    wire::Value v;
    if (c->on.empty() || !wire::DecodeValue(d.payload.data(), d.payload.size(), &v)) return;
    const std::string from = IdText(d.from);
    const std::vector<uint32_t> ids = c->on;  // a callback may close the channel
    for (uint32_t id : ids)
        Invoke(FindCallback(id), [&](lua_State* L) {
            lua_pushlstring(L, from.data(), from.size());
            PushValue(L, v);
            lua_createtable(L, 0, 2);
            lua_pushboolean(L, d.reliable);
            lua_setfield(L, -2, "reliable");
            lua_pushnumber(L, d.at);
            lua_setfield(L, -2, "at");
            return 3;
        });
}

void OnPeer(const peerbus::PeerChange& pc) {
    if (!Running()) return;
    Chan* c = ChanByBus(pc.ch);
    if (!c) return;
    Prune(c->onPeer);
    const std::string id = IdText(pc.id);
    const std::vector<uint32_t> ids = c->onPeer;
    for (uint32_t cb : ids)
        Invoke(FindCallback(cb), [&](lua_State* L) {
            lua_pushlstring(L, id.data(), id.size());
            lua_pushboolean(L, pc.up);
            return 2;
        });
}

void PutBool(json::Value& o, const char* k, bool b) {
    json::Value x;
    x.type = json::Type::Bool;
    x.boolean = b;
    o.members.emplace_back(k, std::move(x));
}
void PutStr(json::Value& o, const char* k, const std::string& s) {
    json::Value x;
    x.type = json::Type::String;
    x.string = s;
    o.members.emplace_back(k, std::move(x));
}

void OnLobby(peerbus::LobbyEvent e) {
    if (!Running()) return;
    peerbus::LobbyView v;
    const bool in = peerbus::Lobby(&v);
    json::Value p;
    p.type = json::Type::Object;
    switch (e) {
        case peerbus::LobbyEvent::Enter:
            PutBool(p, "isHost", in && IsHost(v));
            QueueEvent(ev::kLobbyEnter, std::move(p));
            break;
        case peerbus::LobbyEvent::Leave: QueueEvent(ev::kLobbyLeave, std::move(p)); break;
        case peerbus::LobbyEvent::Members: QueueEvent(ev::kLobbyMembers, std::move(p)); break;
        case peerbus::LobbyEvent::Owner:
            if (in && v.owner) PutStr(p, "owner", IdText(v.owner));
            PutBool(p, "isHost", in && IsHost(v));
            QueueEvent(ev::kLobbyOwner, std::move(p));
            break;
        case peerbus::LobbyEvent::Phase:
            PutStr(p, "phase", in ? v.phase : std::string("none"));
            QueueEvent(ev::kLobbyPhase, std::move(p));
            break;
    }
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kLobby[] = {{"state", LobbyState}, {"isHost", LobbyIsHost}, {nullptr, nullptr}};
    static const luaL_Reg kPeer[] = {{"open", PeerOpen}, {"now", PeerNow}, {nullptr, nullptr}};
    static const luaL_Reg kChannel[] = {{"send", ChSend},         {"broadcast", ChBroadcast},
                                        {"on", ChOn},             {"onPeer", ChOnPeer},
                                        {"peers", ChPeers},       {"clock", ChClock},
                                        {"setPresence", ChSetPresence}, {"presence", ChPresence},
                                        {"close", ChClose},       {nullptr, nullptr}};
    if (luaL_newmetatable(L, kChannelMeta)) {
        lua_newtable(L);
        luaL_setfuncs(L, kChannel, 0);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, &ChToString);
        lua_setfield(L, -2, "__tostring");
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "__metatable");  // getmetatable() hands out nothing to change
    }
    lua_pop(L, 1);
    lua_newtable(L);
    RegisterFunctions(L, -1, kLobby);
    lua_setfield(L, wum, "lobby");
    lua_newtable(L);
    RegisterFunctions(L, -1, kPeer);
    lua_setfield(L, wum, "peer");
    g_chans.clear();  // a new VM: the previous one's channels were closed by their generations' cleanups
    peerbus::Hooks h;
    h.message = &OnMessage;
    h.peer = &OnPeer;
    h.lobby = &OnLobby;
    peerbus::SetHooks(h);
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
