// The game-state channels and methods: `state`, `entities`, `state.get`, `state.vars`, `entities.list`,
// `entity.inspect`. Everything runs on the main thread (subscribe callbacks included).
#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/config_schema.h"
#include "core/events.h"
#include "game/state/gamestate_internal.h"
#include "game/state/gamestate_json.h"
#include "melange/gamestate.h"
#include "melange/oasis.h"
#include "melange/sim.h"
#include "oasis/providers.h"
#include "oasis/state_params.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
namespace gs = melange::gamestate;
namespace sp = stateparams;

struct Sub { int client; uint32_t hz, kinds; uint64_t next; bool sentEmpty; };
ChannelId g_state = 0, g_entities = 0;
std::vector<Sub> g_stateSubs, g_entSubs;
// Global, not per client: a client id is only good for one connection, so keying this by c.client would let a
// reconnect loop reset the limit and force a full DRM walk (the actual main-thread cost this guards) every
// frame instead of at most once a second.
uint64_t g_lastVarsAt = 0;
std::vector<gs::Entity> g_ents(512);
std::vector<gs::Var> g_vars(2048);

void Fail(Result& r, int code, const char* msg) {
    r.ok = false;
    r.code = code;
    r.message = msg;
}

bool NeedGame(Result& r) {
    if (gs::Available()) return true;
    Fail(r, -32001, "game state is unavailable (unrecognised game build, or [GameState] Enabled=0)");
    return false;
}

int ReadEntities() {
    int n = gs::Entities(g_ents.data(), static_cast<int>(g_ents.size()));
    if (n > static_cast<int>(g_ents.size())) {
        g_ents.resize(static_cast<size_t>((std::min)(n, 4096)));
        n = gs::Entities(g_ents.data(), static_cast<int>(g_ents.size()));
    }
    return (std::min)(n, static_cast<int>(g_ents.size()));
}

void OnSub(ChannelId ch, int client, std::string_view filter, bool on, void*) {
    auto& subs = ch == g_state ? g_stateSubs : g_entSubs;
    std::erase_if(subs, [&](const Sub& s) { return s.client == client; });
    if (!on) return;
    const sp::Filter f = ch == g_state ? sp::ParseFilter(filter, 5, 10) : sp::ParseFilter(filter, 2, 5);
    subs.push_back(Sub{client, f.hz, f.kinds, 0, false});
}

void PumpState(uint64_t now) {
    std::string json;
    uint32_t maxHz = 0;
    for (Sub& s : g_stateSubs) {
        maxHz = (std::max)(maxHz, s.hz);
        if (now < s.next) continue;
        s.next = now + 1000 / s.hz;
        if (json.empty()) {
            gs::Snapshot snap{};
            if (!gs::Read(&snap)) snap = gs::Snapshot{};
            json = gs::wire::SnapshotJson(snap, gs::Available());
        }
        PublishTo(g_state, s.client, json);
    }
    if (maxHz) gs::Want(maxHz);
}

void PumpEntities(uint64_t now) {
    int n = -1;
    std::map<uint32_t, std::string> byKinds;
    for (Sub& s : g_entSubs) {
        if (now < s.next) continue;
        s.next = now + 1000 / s.hz;
        if (n < 0) n = ReadEntities();
        if (!n) {
            if (!s.sentEmpty) PublishTo(g_entities, s.client, "[]");
            s.sentEmpty = true;
            continue;
        }
        s.sentEmpty = false;
        auto it = byKinds.find(s.kinds);
        if (it == byKinds.end()) it = byKinds.emplace(s.kinds, gs::wire::EntitiesJson(g_ents.data(), n, s.kinds)).first;
        PublishTo(g_entities, s.client, it->second);
    }
}

void OnFrame() {
    const bool st = HasSubscribers(g_state), en = HasSubscribers(g_entities);
    if (!st && !en) return;
    const uint64_t now = GetTickCount64();
    if (st) PumpState(now);
    if (en) PumpEntities(now);
}

void StateGet(const Call&, Result& r, void*) {
    if (!NeedGame(r)) return;
    gs::Snapshot s{};
    if (!gs::Read(&s)) return Fail(r, -32001, "game state could not be read");
    r.json = gs::wire::SnapshotJson(s, true);
}

void StateVars(const Call& c, Result& r, void*) {
    if (!NeedGame(r)) return;
    std::string prefix;
    if (!sp::ParsePrefix(c.paramsJson, &prefix)) return Fail(r, -32602, "prefix must be a string of at most 63 characters");
    const uint64_t now = GetTickCount64();
    if (g_lastVarsAt && now - g_lastVarsAt < 1000) return Fail(r, -32002, "state.vars: at most one call per second");
    g_lastVarsAt = now;
    const char* p = prefix.empty() ? nullptr : prefix.c_str();
    int n = gs::Vars(g_vars.data(), static_cast<int>(g_vars.size()), p);
    if (n > static_cast<int>(g_vars.size())) {
        g_vars.resize(static_cast<size_t>((std::min)(n, 16384)));
        n = gs::Vars(g_vars.data(), static_cast<int>(g_vars.size()), p);
    }
    n = (std::min)(n, static_cast<int>(g_vars.size()));
    jsonmini::Arr a;
    for (int i = 0; i < n; ++i) a.Raw(gs::wire::VarJson(g_vars[static_cast<size_t>(i)]));
    r.json = a.End();
}

void EntitiesList(const Call& c, Result& r, void*) {
    if (!NeedGame(r)) return;
    if (!sim::InMatch()) return Fail(r, -32001, "not in a match");
    json::Value v;
    json::Error e;
    uint32_t kinds = gs::wire::kAllKinds;
    if (json::Parse(c.paramsJson, &v, &e, 4096) && v.IsObject()) kinds = sp::Kinds(v.Get("kinds"));
    r.json = gs::wire::EntitiesJson(g_ents.data(), ReadEntities(), kinds);
}

std::string HexAt(uintptr_t addr, uint32_t len) {
    std::vector<uint8_t> bytes(len);
    std::vector<uint8_t> ok(len, 1);
    if (!gs::Peek(addr, bytes.data(), len)) {
        for (uint32_t off = 0; off < len;) {
            const uint32_t chunk = (std::min)(len - off, static_cast<uint32_t>(0x1000 - ((addr + off) & 0xfff)));
            const bool good = gs::Peek(addr + off, bytes.data() + off, chunk);
            std::fill(ok.begin() + off, ok.begin() + off + chunk, static_cast<uint8_t>(good));
            off += chunk;
        }
    }
    return "\"" + sp::Hex(bytes.data(), ok.data(), len) + "\"";
}

template <class T>
bool At(uintptr_t a, T* out) {
    return gs::Peek(a, out, sizeof(T));
}

std::string Fields(const gs::Entity& e) {
    jsonmini::Obj o;
    if (e.hasPos) o.Raw("pos", gs::wire::VecJson(e.pos)).Raw("vel", gs::wire::VecJson(e.vel));
    if (e.label[0]) o.Str("label", e.label);
    switch (e.kind) {
        case gs::EntityKind::Worm: {
            uint8_t slot = 0xff;
            gs::Snapshot s{};
            if (!At(e.object + 0x30, &slot) || !gs::Read(&s)) break;
            o.Int("slot", slot);
            for (int i = 0; i < s.wormCount; ++i) {
                const gs::Worm& w = s.worms[i];
                if (w.slot != slot) continue;
                o.Int("team", w.team).Int("health", w.health).Int("physicsState", w.physicsState).Int("weapon", w.weapon);
                o.Bool("active", w.active).Bool("alive", w.alive);
            }
            break;
        }
        case gs::EntityKind::Projectile: {
            int32_t fuse = 0;
            if (At(e.object + 0x58, &fuse)) o.Int("fuse", fuse);
            break;
        }
        case gs::EntityKind::Barrel: {
            uint8_t exploded = 0;
            if (At(e.object + 0x68, &exploded)) o.Bool("exploded", exploded != 0);
            break;
        }
        default: break;
    }
    return o.End();
}

void EntityInspect(const Call& c, Result& r, void*) {
    if (!NeedGame(r)) return;
    const sp::Inspect q = sp::ParseInspect(c.paramsJson);
    if (!q.error.empty()) return Fail(r, -32602, q.error.c_str());
    const bool raw = config::GetBool("Oasis", "RawInspect", true);
    jsonmini::Obj o;
    if (q.byHandle) {
        if (!sim::InMatch()) return Fail(r, -32001, "not in a match");
        const int n = ReadEntities();
        const gs::Entity* e = nullptr;
        for (int i = 0; i < n && !e; ++i)
            if (g_ents[static_cast<size_t>(i)].handle == q.handle) e = &g_ents[static_cast<size_t>(i)];
        if (!e) return Fail(r, -32602, "no live entity with that handle");
        o.UInt("handle", e->handle).UInt("addr", e->object).UInt("len", q.len).UInt("vtable", e->vtable);
        o.Str("type", e->type).Str("kind", gs::wire::KindName(e->kind)).Raw("fields", Fields(*e));
        o.Raw("hex", raw ? HexAt(e->object, q.len) : "null");
    } else {
        if (!raw) return Fail(r, -32000, "the raw memory view is off ([Oasis] RawInspect=0)");
        uintptr_t vt = 0;
        char type[48] = "";
        bool payload = false;
        if (At(q.addr, &vt)) gs::detail::Rtti(vt, type, sizeof type, &payload);
        o.UInt("addr", q.addr).UInt("len", q.len).UInt("vtable", vt).Str("type", type).Raw("fields", "{}");
        o.Raw("hex", HexAt(q.addr, q.len));
    }
    r.json = o.End();
}
}  // namespace

void InstallState() {
    ChannelOptions co;
    co.overflow = Overflow::Coalesce;
    co.mainThreadSubscribe = true;
    g_state = AddChannel("state", co);
    g_entities = AddChannel("entities", co);
    OnSubscribe(g_state, &OnSub, nullptr);
    OnSubscribe(g_entities, &OnSub, nullptr);
    AddMethod("state.get", &StateGet, nullptr, kRpcGameOnly);
    AddMethod("state.vars", &StateVars, nullptr, kRpcGameOnly);
    events::Subscribe(events::Event::Frame, &OnFrame);
}

void InstallEntities() {
    AddMethod("entities.list", &EntitiesList, nullptr, kRpcGameOnly);
    AddMethod("entity.inspect", &EntityInspect, nullptr, kRpcGameOnly);
    config::schema::MarkLive("Oasis", "RawInspect");
}
}  // namespace melange::oasis::providers
