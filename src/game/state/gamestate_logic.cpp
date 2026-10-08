#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "game/state/gamestate_internal.h"

namespace melange::gamestate::detail {
namespace {
// Descriptor classes of the data store, by vtable; their GetType (slot 4) returns the VarType value.
constexpr struct { uintptr_t vt; VarType type; } kVarVtables[] = {
    {0x887b74, VarType::Int},    {0x887bbc, VarType::Uint},        {0x887c04, VarType::Float},
    {0x887c4c, VarType::Vector}, {0x887cdc, VarType::String},      {0x887d74, VarType::Container},
    {0x887d24, VarType::StringTable}, {0x887c94, VarType::Color},
};

constexpr uint16_t kCp1252[32] = {0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd, 0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
                                  0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178};

// Length of the valid UTF-8 sequence at s (strict: no overlongs, no surrogates), or 0.
size_t Utf8Len(const unsigned char* s, size_t n) {
    const unsigned char c = s[0];
    size_t k;
    unsigned char lo = 0x80, hi = 0xbf;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) k = 1;
    else if (c >= 0xe0 && c <= 0xef) k = 2, lo = c == 0xe0 ? 0xa0 : 0x80, hi = c == 0xed ? 0x9f : 0xbf;
    else if (c >= 0xf0 && c <= 0xf4) k = 3, lo = c == 0xf0 ? 0x90 : 0x80, hi = c == 0xf4 ? 0x8f : 0xbf;
    else return 0;
    if (k >= n) return 0;
    for (size_t j = 1; j <= k; ++j) {
        const unsigned char b = s[j];
        if (j == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xbf)) return 0;
    }
    return k + 1;
}

size_t Encode(uint32_t cp, char* o) {
    if (cp < 0x80) return o[0] = static_cast<char>(cp), 1;
    if (cp < 0x800) {
        o[0] = static_cast<char>(0xc0 | (cp >> 6));
        o[1] = static_cast<char>(0x80 | (cp & 0x3f));
        return 2;
    }
    o[0] = static_cast<char>(0xe0 | (cp >> 12));
    o[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    o[2] = static_cast<char>(0x80 | (cp & 0x3f));
    return 3;
}

float Finite(float v) { return std::isfinite(v) ? v : 0.f; }

Vec3 ReadVec(uintptr_t a) {
    Vec3 v{};
    if (!Copy(a, &v, sizeof v)) return Vec3{};
    return Vec3{Finite(v.x), Finite(v.y), Finite(v.z)};
}

bool Plausible(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) < 1e6f &&
           std::fabs(v.y) < 1e6f && std::fabs(v.z) < 1e6f;
}

template <class T>
bool Value(const Engine& e, const char* name, VarType want, T* out) {
    if (!e.getResource) return false;
    const uintptr_t d = e.getResource(name);
    if (!d) return false;
    bool ok = false;
    if (TypeOfDescriptor(d) == want) ok = Copy(Rd<uintptr_t>(d + 4) + 0x1c, out, sizeof(T));
    if (e.release) e.release(d);
    return ok;
}

bool StringValue(const Engine& e, const char* name, char* out, size_t cap) {
    uintptr_t p = 0;
    if (!Value(e, name, VarType::String, &p)) return false;
    ReadCString(p, out, cap);
    return true;
}

void Append(char* out, size_t cap, size_t* len, const char* s) {
    const size_t n = strlen(s);
    if (*len + n >= cap) return;
    memcpy(out + *len, s, n + 1);
    *len += n;
}

// A JSON string literal of `s` (UTF-8) that fits in cap bytes with its terminator, cut on a character boundary.
void JsonString(const char* s, char* out, size_t cap) {
    size_t len = 0;
    out[len++] = '"';
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    const size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        char piece[8];
        size_t k = Utf8Len(p + i, n - i);
        if (!k) k = 1;
        size_t pl = 0;
        const unsigned char c = p[i];
        if (c == '"' || c == '\\') piece[0] = '\\', piece[1] = static_cast<char>(c), pl = 2;
        else if (c < 0x20) pl = static_cast<size_t>(snprintf(piece, sizeof piece, "\\u%04x", c));
        else memcpy(piece, p + i, k), pl = k;
        if (len + pl + 2 > cap) break;
        memcpy(out + len, piece, pl);
        len += pl;
        i += k;
    }
    out[len++] = '"';
    out[len] = 0;
}

void Num(char* out, size_t cap, size_t* len, float v) {
    char b[32];
    if (std::isfinite(v)) snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    else snprintf(b, sizeof b, "null");
    Append(out, cap, len, b);
}
}  // namespace

bool Copy(uintptr_t addr, void* out, size_t n) {
    if (!n) return true;
    if (!addr) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PeekGuarded(uintptr_t addr, void* out, uint32_t n) {
    if (!out || n > 4096) return false;
    if (!n) return true;
    const uintptr_t end = addr + n;
    if (!addr || end < addr) return false;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    for (uintptr_t p = addr; p < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & kReadable))
            return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) return false;
        p = next;
    }
    return Copy(addr, out, n);
}

void Utf8(std::string_view in, char* out, size_t cap) {
    if (!cap) return;
    size_t len = 0;
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    for (size_t i = 0; i < in.size() && s[i];) {
        char piece[4];
        size_t pl, used;
        if (const size_t k = Utf8Len(s + i, in.size() - i)) {
            memcpy(piece, s + i, k);
            pl = used = k;
        } else {
            const unsigned char c = s[i];
            pl = Encode(c < 0xa0 ? kCp1252[c - 0x80] : c, piece);
            used = 1;
        }
        if (len + pl >= cap) break;
        memcpy(out + len, piece, pl);
        len += pl;
        i += used;
    }
    out[len] = 0;
}

void ReadCString(uintptr_t p, char* out, size_t cap) {
    char buf[256];
    size_t n = 0;
    while (p && n < sizeof buf - 1) {
        const size_t room = sizeof buf - 1 - n;
        const size_t toPage = 0x1000 - ((p + n) & 0xfff);
        const size_t chunk = room < toPage ? room : toPage;
        if (!Copy(p + n, buf + n, chunk)) break;
        const void* z = memchr(buf + n, 0, chunk);
        if (z) {
            n = static_cast<size_t>(static_cast<const char*>(z) - buf);
            break;
        }
        n += chunk;
    }
    Utf8(std::string_view(buf, n), out, cap);
}

uintptr_t Container(uintptr_t handles, int i, uintptr_t vt) {
    const uintptr_t h = Rd<uintptr_t>(handles + 4 * static_cast<uintptr_t>(i));
    if (!h) return 0;
    const uintptr_t inner = Rd<uintptr_t>(h + 4);
    if (!inner) return 0;
    const uintptr_t c = Rd<uintptr_t>(inner + 0x1c);
    return c && Rd<uintptr_t>(c) == vt ? c : 0;
}

void FillSnapshot(const Layout& l, const Engine& e, const MatchInfo& m, Snapshot* out) {
    const uint64_t frame = out->frame;
    *out = Snapshot{};
    out->frame = frame;
    out->matchSerial = m.serial;
    Match& mt = out->match;
    mt.inMatch = m.inMatch;
    mt.online = m.online;
    mt.turnsStarted = m.turnsStarted;
    mt.suddenDeath = m.suddenDeath;
    mt.currentTeam = mt.activeWorm = -1;
    if (!m.inMatch) return;

    int32_t v = -1;
    if (e.getInt && e.getInt("CurrentTeamIndex", &v) && v >= 0 && v < 4) mt.currentTeam = v;
    v = -1;
    if (e.getInt && e.getInt("ActiveWormIndex", &v) && v >= 0 && v < 16) mt.activeWorm = v;
    Value(e, "TurnTime", VarType::Int, &mt.turnMs);
    Value(e, "TurnTimeRemaining", VarType::Int, &mt.turnMsLeft);
    Value(e, "RoundTime", VarType::Int, &mt.roundMs);
    Value(e, "RoundTimeRemaining", VarType::Int, &mt.roundMsLeft);
    Value(e, "Wind.Speed", VarType::Float, &mt.windSpeed);
    Value(e, "Wind.Direction", VarType::Float, &mt.windDir);
    Value(e, "Water.Level", VarType::Float, &mt.waterLevel);
    mt.windSpeed = Finite(mt.windSpeed);
    mt.windDir = Finite(mt.windDir);
    mt.waterLevel = Finite(mt.waterLevel);
    StringValue(e, "Land.Theme", mt.theme, sizeof mt.theme);

    for (int i = 0; i < 4; ++i) {
        const uintptr_t c = Container(l.teamHandles, i, l.teamVt);
        if (!c) continue;
        Team t{};
        t.slot = static_cast<uint8_t>(i);
        ReadCString(Rd<uintptr_t>(c + 0x14), t.name, sizeof t.name);
        t.active = Rd<uint8_t>(c + 0x68) != 0;
        if (!t.active && !t.name[0]) continue;
        t.colour = Rd<uint8_t>(c + 0x6a);
        t.alliance = Rd<uint8_t>(c + 0x6c);
        t.roundsWon = Rd<uint8_t>(c + 0x6f);
        t.ai = Rd<uint8_t>(c + 0x6e) != 0;
        t.local = Rd<uint8_t>(c + 0x75) != 0;
        t.score = static_cast<int32_t>(Rd<uint32_t>(c + 0x48));
        out->teams[out->teamCount++] = t;
    }
    for (int i = 0; i < 16; ++i) {
        const uintptr_t c = Container(l.wormHandles, i, l.wormVt);
        if (!c) continue;
        Worm w{};
        w.slot = static_cast<uint8_t>(i);
        ReadCString(Rd<uintptr_t>(c + 0xb0), w.name, sizeof w.name);
        w.active = Rd<uint8_t>(c + 0x124) != 0;
        if (!w.active && !w.name[0]) continue;
        w.team = Rd<uint8_t>(c + 0x127);
        w.posInTeam = Rd<uint8_t>(c + 0x128);
        const uint32_t ps = Rd<uint32_t>(c + 0xf0);
        w.physicsState = static_cast<uint8_t>(ps < 256 ? ps : 255);
        w.alive = w.active && w.physicsState != 8;
        w.health = Rd<uint16_t>(c + 0x11e);
        const int32_t wi = Rd<int32_t>(c + 0xf4);
        w.weapon = static_cast<int16_t>(wi >= 1 && wi <= 65 ? wi : -1);
        w.pos = ReadVec(c + 0x38);
        w.vel = ReadVec(c + 0x50);
        w.yaw = Finite(Rd<float>(c + 0x90));
        out->worms[out->wormCount++] = w;
    }
}

VarType TypeOfDescriptor(uintptr_t desc) {
    const uintptr_t vt = Rd<uintptr_t>(desc);
    for (const auto& k : kVarVtables)
        if (k.vt == vt) return k.type;
    return VarType::Undefined;
}

bool NameOf(uintptr_t desc, char* out, size_t cap) {
    out[0] = 0;
    const uintptr_t h = Rd<uintptr_t>(desc + 4);
    if (!h) return false;
    ReadCString(Rd<uintptr_t>(h + 0x14), out, cap);
    return out[0] != 0;
}

bool ReadVar(uintptr_t desc, Var* out) {
    *out = Var{};
    if (!NameOf(desc, out->name, sizeof out->name)) return false;
    out->type = TypeOfDescriptor(desc);
    const uintptr_t at = Rd<uintptr_t>(desc + 4) + 0x1c;
    char* v = out->value;
    const size_t cap = sizeof out->value;
    size_t len = 0;
    v[0] = 0;
    switch (out->type) {
        case VarType::Int: snprintf(v, cap, "%d", Rd<int32_t>(at)); break;
        case VarType::Uint: snprintf(v, cap, "%u", Rd<uint32_t>(at)); break;
        case VarType::Float: Num(v, cap, &len, Rd<float>(at)); break;
        case VarType::Vector: {
            float f[3] = {};
            Copy(at, f, sizeof f);
            Append(v, cap, &len, "[");
            for (int i = 0; i < 3; ++i) {
                if (i) Append(v, cap, &len, ",");
                Num(v, cap, &len, f[i]);
            }
            Append(v, cap, &len, "]");
            break;
        }
        case VarType::String: {
            char s[256];
            ReadCString(Rd<uintptr_t>(at), s, sizeof s);
            JsonString(s, v, cap);
            break;
        }
        case VarType::Container: {
            const uintptr_t p = Rd<uintptr_t>(at);
            if (!p) {
                snprintf(v, cap, "null");
                break;
            }
            char cls[48] = "";
            bool payload = false;
            Rtti(Rd<uintptr_t>(p), cls, sizeof cls, &payload);
            char q[64];
            JsonString(cls, q, sizeof q);
            snprintf(v, cap, "{\"addr\":%u,\"class\":%s}", static_cast<unsigned>(p), q);
            break;
        }
        case VarType::Color: snprintf(v, cap, "\"#%08x\"", Rd<uint32_t>(at)); break;
        default: snprintf(v, cap, "null"); break;
    }
    return true;
}

int EnumerateVars(const Engine& e, Var* out, int max, const char* prefix) {
    if (!e.enumerate) return -1;
    struct Ctx { Var* out; int max, n, visited; const char* prefix; size_t plen; } ctx{out, max, 0, 0, prefix, prefix ? strlen(prefix) : 0};
    const EnumCb cb = [](uintptr_t desc, void* p) -> bool {
        auto& c = *static_cast<Ctx*>(p);
        if (++c.visited > 200000) return false;
        char name[64];
        if (!NameOf(desc, name, sizeof name)) return true;
        if (c.plen && strncmp(name, c.prefix, c.plen) != 0) return true;
        if (c.n < c.max && c.out) ReadVar(desc, &c.out[c.n]);
        ++c.n;
        return true;
    };
    return e.enumerate(cb, &ctx) ? ctx.n : -1;
}

bool ReadVar1(const Engine& e, const char* name, Var* out) {
    if (!name || !*name || !e.getResource) return false;
    const uintptr_t d = e.getResource(name);
    if (!d) return false;
    const bool ok = ReadVar(d, out);
    if (e.release) e.release(d);
    return ok;
}

std::string Demangle(std::string_view raw) {
    if (raw.size() < 5 || raw.substr(0, 3) != ".?A") return std::string(raw);
    std::string_view s = raw.substr(4);
    if (s.size() >= 2 && s.substr(s.size() - 2) == "@@") s.remove_suffix(2);
    if (s.find('?') != std::string_view::npos) return std::string(s);
    std::string out;
    while (!s.empty()) {
        const size_t at = s.rfind('@');
        const std::string_view part = at == std::string_view::npos ? s : s.substr(at + 1);
        if (!out.empty()) out += "::";
        out += part;
        if (at == std::string_view::npos) break;
        s = s.substr(0, at);
    }
    return out;
}

namespace {
uintptr_t g_rttiLo = 0, g_rttiHi = UINTPTR_MAX;
bool InRange(uintptr_t p, size_t n) { return p >= g_rttiLo && p <= g_rttiHi && g_rttiHi - p >= n; }
}  // namespace

void SetRttiRange(uintptr_t lo, uintptr_t hi) {
    g_rttiLo = lo;
    g_rttiHi = hi;
}

bool Rtti(uintptr_t vt, char* name, size_t cap, bool* payload) {
    *payload = false;
    if (cap) name[0] = 0;
    if (vt < 4 || !InRange(vt - 4, 4)) return false;
    const uintptr_t col = Rd<uintptr_t>(vt - 4);
    if (!col || !InRange(col, 20) || Rd<uint32_t>(col) != 0) return false;
    const uintptr_t td = Rd<uintptr_t>(col + 12), chd = Rd<uintptr_t>(col + 16);
    if (!InRange(td, 12) || !InRange(chd, 16)) return false;
    char raw[128];
    ReadCString(td + 8, raw, sizeof raw);
    if (strncmp(raw, ".?A", 3) != 0) return false;
    const uint32_t nb = Rd<uint32_t>(chd + 8);
    const uintptr_t bca = Rd<uintptr_t>(chd + 12);
    for (uint32_t i = 0; nb <= 64 && i < nb && InRange(bca, 4 * nb); ++i) {
        const uintptr_t bcd = Rd<uintptr_t>(bca + 4 * i);
        const uintptr_t btd = InRange(bcd, 4) ? Rd<uintptr_t>(bcd) : 0;
        if (!InRange(btd, 12)) continue;
        char b[64];
        ReadCString(btd + 8, b, sizeof b);
        if (strcmp(b, ".?AVPayloadLogicEntity@@") == 0) *payload = true;
    }
    Utf8(Demangle(raw), name, cap);
    return true;
}

ClassInfo Classify(uintptr_t vt) {
    static std::unordered_map<uintptr_t, ClassInfo> cache;
    if (auto it = cache.find(vt); it != cache.end()) return it->second;
    if (cache.size() > 4096) cache.clear();
    ClassInfo ci{EntityKind::Other, ""};
    bool payload = false;
    if (Rtti(vt, ci.type, sizeof ci.type, &payload)) {
        if (strcmp(ci.type, "WXWormLogicEntity") == 0) ci.kind = EntityKind::Worm;
        else if (payload) ci.kind = EntityKind::Projectile;
        else if (strcmp(ci.type, "CrateLogicEntity") == 0) ci.kind = EntityKind::Crate;
        else if (strcmp(ci.type, "OilDrumLogicEntity") == 0) ci.kind = EntityKind::Barrel;
    } else {
        snprintf(ci.type, sizeof ci.type, "vtbl:0x%08x", static_cast<unsigned>(vt));
    }
    cache.emplace(vt, ci);
    return ci;
}

int WalkEntities(const Layout& l, Entity* out, int max) {
    const uintptr_t tm = Rd<uintptr_t>(l.taskManager);
    const uintptr_t tbl = tm ? Rd<uintptr_t>(tm + 0x1c) : 0;
    const uintptr_t base = tbl ? Rd<uintptr_t>(tbl) : 0;
    const uint32_t cap = tbl ? Rd<uint16_t>(tbl + 0x16) : 0;
    if (!base || !cap || cap > 4096) return 0;
    static std::vector<uint8_t> buf;
    buf.resize(static_cast<size_t>(cap) * 0x24);
    if (!Copy(base, buf.data(), buf.size())) return 0;
    int total = 0, n = 0;
    for (uint32_t i = 0; i < cap; ++i) {
        const uint8_t* e = buf.data() + static_cast<size_t>(i) * 0x24;
        uint16_t freeFlag;
        uint32_t handle;
        uintptr_t obj;
        memcpy(&freeFlag, e + 8, 2);
        memcpy(&handle, e + 0x14, 4);
        memcpy(&obj, e + 0xc, 4);
        if (freeFlag || (handle & 0xfff) != i || !obj) continue;
        const uintptr_t vt = Rd<uintptr_t>(obj);
        if (!vt) continue;
        ++total;
        if (n >= max || !out) continue;
        Entity& x = out[n++];
        x = Entity{};
        x.handle = handle;
        x.object = obj;
        x.vtable = vt;
        const ClassInfo ci = Classify(vt);
        x.kind = ci.kind;
        memcpy(x.type, ci.type, sizeof x.type);
        switch (ci.kind) {
            case EntityKind::Worm: {
                const uint8_t slot = Rd<uint8_t>(obj + 0x30);
                const uintptr_t c = slot < 16 ? Container(l.wormHandles, slot, l.wormVt) : 0;
                if (!c) break;
                x.pos = ReadVec(c + 0x38);
                x.vel = ReadVec(c + 0x50);
                x.hasPos = true;
                ReadCString(Rd<uintptr_t>(c + 0xb0), x.label, sizeof x.label);
                break;
            }
            case EntityKind::Projectile: {
                x.pos = ReadVec(obj + 0x28);
                x.vel = ReadVec(obj + 0x34);
                x.hasPos = true;
                const uintptr_t d = Rd<uintptr_t>(obj + 0xc4);
                const uintptr_t h = d ? Rd<uintptr_t>(d + 4) : 0;
                char w[48];
                ReadCString(h ? Rd<uintptr_t>(h + 0x14) : 0, w, sizeof w);
                const char* name = strncmp(w, "kWeapon", 7) == 0 ? w + 7 : w;
                Utf8(name, x.label, sizeof x.label);
                break;
            }
            case EntityKind::Crate: x.pos = ReadVec(obj + 0x2c), x.hasPos = true; break;
            case EntityKind::Barrel: x.pos = ReadVec(obj + 0x20), x.hasPos = true; break;
            default: break;
        }
        if (x.hasPos && !Plausible(x.pos)) x.hasPos = false;
    }
    return total;
}

bool InWorld(const Vec3& v) { return Plausible(v); }

bool FrameBudget::Take(uint64_t now, int cap) {
    if (now != frame) frame = now, used = 0;
    if (used >= cap) return false;
    ++used;
    return true;
}

// The segment is swept over its first kLandRayMaxLength units in the engine's 1000 steps, and the hit time is mapped
// back onto a->b. The engine accepts a hit up to one step past the end of its sweep: anything beyond the segment misses.
LandRayResult SegmentLandRay(LandSweep sweep, const Vec3& a, const Vec3& b, LandHit* out) {
    if (!out || !InWorld(a) || !InWorld(b)) return LandRayResult::Invalid;
    const Vec3 d{b.x - a.x, b.y - a.y, b.z - a.z};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(len >= 1e-3f)) return LandRayResult::Miss;
    const float frac = len > kLandRayMaxLength ? kLandRayMaxLength / len : 1.f;
    const float k = frac / static_cast<float>(kLandRayTicks);
    RawLandHit r{};
    if (!sweep || !sweep(a, Vec3{d.x * k, d.y * k, d.z * k}, kLandRayTicks, &r)) return LandRayResult::Unavailable;
    if (!r.hit || !std::isfinite(r.time) || r.time > static_cast<float>(kLandRayTicks)) return LandRayResult::Miss;
    const float s = r.time > 0.f ? r.time / static_cast<float>(kLandRayTicks) : 0.f;
    out->t = (std::min)(s * frac, 1.f);
    const Vec3& n = r.normal;
    const float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);  // NaN or inf for a bad normal
    if (std::isfinite(nl) && nl > 1e-6f) out->normal = Vec3{n.x / nl, n.y / nl, n.z / nl};
    else out->normal = Vec3{-d.x / len, -d.y / len, -d.z / len};  // no usable normal: face the ray
    return LandRayResult::Hit;
}
}  // namespace melange::gamestate::detail
