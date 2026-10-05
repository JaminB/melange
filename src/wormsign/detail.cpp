#include "wormsign/detail.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "wormsign/hash_engine.h"

namespace melange::wormsign::detail {
namespace {
constexpr size_t kArena = 1152 * 1024;

std::mutex g_mu;
uint8_t g_arena[kArena];
struct Idx {
    uint32_t tick, off;
    uint16_t len;
    bool valid;
};
Idx g_idx[kRingTicks];
uint32_t g_pos = 0;
DetailRec g_scratch;

struct Out {
    uint8_t* p;
    size_t cap, n = 0;
    bool ok = true;
    void Put(const void* v, size_t k) {
        if (!ok || n + k > cap) {
            ok = false;
            return;
        }
        memcpy(p + n, v, k);
        n += k;
    }
    template <class T>
    void V(const T& v) { Put(&v, sizeof v); }
};
struct In {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    void Get(void* v, size_t k) {
        if (!ok || i + k > n) {
            ok = false;
            return;
        }
        memcpy(v, p + i, k);
        i += k;
    }
    template <class T>
    T V() {
        T v{};
        Get(&v, sizeof v);
        return v;
    }
};

enum Kind : uint8_t { F3, U8, U16, U32, I32, H32 };
struct Field {
    const char* name;
    uint8_t pos;
    Kind kind;
};
constexpr Field kWorm[] = {
    {"normal", 0, F3},     {"aftertouch", 12, F3}, {"pos", 24, F3},          {"vel", 36, F3},     {"rot", 48, F3},
    {"f_cc", 60, U16},     {"f_d0", 62, H32},      {"f_d4", 66, U8},         {"f_d8", 67, H32},   {"f_e0", 71, H32},
    {"energySrc", 75, U16}, {"physicsState", 77, U32}, {"weapon", 81, I32}, {"f_f8", 85, U16},   {"f_10c", 87, U8},
    {"f_110", 88, U8},     {"energy", 89, U16},    {"active", 91, U8},       {"team", 92, U8},
};

std::string Num(float f) {
    if (std::isnan(f)) return "\"nan\"";
    if (std::isinf(f)) return f > 0 ? "\"inf\"" : "\"-inf\"";
    char b[32];
    snprintf(b, sizeof b, "%.9g", static_cast<double>(f));
    return b;
}
float F(const uint8_t* p) {
    float f;
    memcpy(&f, p, 4);
    return f;
}
float Fb(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
std::string Scalar(const uint8_t* p, Kind k) {
    char b[24];
    uint16_t u16;
    uint32_t u32;
    switch (k) {
        case U8: snprintf(b, sizeof b, "%u", p[0]); break;
        case U16: memcpy(&u16, p, 2), snprintf(b, sizeof b, "%u", u16); break;
        case U32: memcpy(&u32, p, 4), snprintf(b, sizeof b, "%u", u32); break;
        case I32: memcpy(&u32, p, 4), snprintf(b, sizeof b, "%d", static_cast<int32_t>(u32)); break;
        case H32: memcpy(&u32, p, 4), snprintf(b, sizeof b, "\"0x%08x\"", u32); break;
        default: b[0] = 0;
    }
    return b;
}
size_t Width(Kind k) { return k == F3 ? 12 : k == U8 ? 1 : k == U16 ? 2 : 4; }
std::string Hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "\"0x%08x\"", v);
    return b;
}
std::string Unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"') s = s.substr(1, s.size() - 2);
    return s;
}
std::string Vec(const uint32_t* bits) {
    return "[" + Num(Fb(bits[0])) + "," + Num(Fb(bits[1])) + "," + Num(Fb(bits[2])) + "]";
}
const char* kAxis[3] = {".x", ".y", ".z"};

const WormDetail* FindWorm(const DetailRec& r, uint8_t slot) {
    for (int i = 0; i < r.wormCount && i < 16; ++i)
        if ((r.worms[i].slot & 0x7f) == slot) return &r.worms[i];
    return nullptr;
}
}  // namespace

void Clear(DetailRec* r) {
    r->tick = 0;
    r->wormCount = 0;
    r->projCount = 0;
    for (auto& t : r->teams) t = {kNoTeam, 0, 0};
    r->rng = r->rng2 = 0;
    r->curTeam = r->activeWorm = 0;
    r->cam = CameraDetail{};
}

size_t CameraHashBytes(const CameraDetail& c, uint8_t out[kCameraHashBytes]) {
    out[0] = c.flags;
    memcpy(out + 1, c.pos, 12);
    memcpy(out + 13, c.target, 12);
    memcpy(out + 25, c.up, 12);
    return kCameraHashBytes;
}

uint64_t CameraHash(const CameraDetail& c) {
    uint8_t b[kCameraHashBytes];
    return Fnv(b, CameraHashBytes(c, b));
}

DetailRec* Scratch() {
    Clear(&g_scratch);
    return &g_scratch;
}

void Commit(const DetailRec& r) {
    uint8_t buf[kMaxPacked];
    const size_t len = Pack(r, buf, sizeof buf);
    if (!len) return;
    std::lock_guard lk(g_mu);
    if (g_pos + len > kArena) g_pos = 0;
    const uint32_t a = g_pos, b = static_cast<uint32_t>(g_pos + len);
    for (auto& e : g_idx)
        if (e.valid && e.off < b && e.off + e.len > a) e.valid = false;
    memcpy(g_arena + a, buf, len);
    g_idx[r.tick % kRingTicks] = {r.tick, a, static_cast<uint16_t>(len), true};
    g_pos = b;
}

void Reset() {
    std::lock_guard lk(g_mu);
    for (auto& e : g_idx) e.valid = false;
    g_pos = 0;
}

bool GetPacked(uint32_t tick, std::vector<uint8_t>* out) {
    std::lock_guard lk(g_mu);
    const Idx& e = g_idx[tick % kRingTicks];
    if (!e.valid || e.tick != tick) return false;
    out->assign(g_arena + e.off, g_arena + e.off + e.len);
    return true;
}

bool Get(uint32_t tick, DetailRec* out) {
    uint8_t buf[kMaxPacked];
    size_t len = 0;
    {
        std::lock_guard lk(g_mu);
        const Idx& e = g_idx[tick % kRingTicks];
        if (!e.valid || e.tick != tick) return false;
        len = e.len;
        memcpy(buf, g_arena + e.off, len);
    }
    return Unpack(buf, len, out);
}

size_t RingBytes() { return sizeof g_arena + sizeof g_idx; }

size_t Pack(const DetailRec& r, uint8_t* out, size_t cap) {
    Out o{out, cap};
    o.V(r.tick), o.V(r.rng), o.V(r.rng2), o.V(r.curTeam), o.V(r.activeWorm);
    const uint8_t wc = r.wormCount > 16 ? 16 : r.wormCount;
    o.V(wc);
    for (int i = 0; i < wc; ++i) o.V(r.worms[i].slot), o.Put(r.worms[i].bytes, kWormBytes);
    uint8_t tc = 0;
    for (auto& t : r.teams) tc += t.slot != kNoTeam;
    o.V(tc);
    for (auto& t : r.teams)
        if (t.slot != kNoTeam) o.V(t.slot), o.V(t.active), o.V(t.score);
    const uint16_t pc = r.projCount > 64 ? 64 : r.projCount;
    o.V(pc);
    for (int i = 0; i < pc; ++i) {
        const ProjDetail& p = r.proj[i];
        o.V(p.vt), o.V(p.time), o.V(p.cat), o.Put(p.pv, sizeof p.pv);
    }
    const CameraDetail& c = r.cam;
    o.V(c.flags), o.V(c.index), o.V(c.count), o.V(c.view);
    o.Put(c.pos, sizeof c.pos), o.Put(c.target, sizeof c.target), o.Put(c.up, sizeof c.up);
    return o.ok ? o.n : 0;
}

bool Unpack(const uint8_t* p, size_t n, DetailRec* r) {
    In in{p, n};
    Clear(r);
    r->tick = in.V<uint32_t>();
    r->rng = in.V<uint32_t>();
    r->rng2 = in.V<uint32_t>();
    r->curTeam = in.V<int32_t>();
    r->activeWorm = in.V<int32_t>();
    const uint8_t wc = in.V<uint8_t>();
    if (wc > 16) return false;
    r->wormCount = wc;
    for (int i = 0; i < wc; ++i) {
        r->worms[i].slot = in.V<uint8_t>();
        in.Get(r->worms[i].bytes, kWormBytes);
    }
    const uint8_t tc = in.V<uint8_t>();
    if (tc > 4) return false;
    for (int i = 0; i < tc; ++i) {
        TeamDetail& t = r->teams[i];
        t.slot = in.V<uint8_t>();
        t.active = in.V<uint8_t>();
        t.score = in.V<uint32_t>();
        if (t.slot == kNoTeam) return false;
    }
    const uint16_t pc = in.V<uint16_t>();
    if (pc > 64) return false;
    r->projCount = pc;
    for (int i = 0; i < pc; ++i) {
        ProjDetail& d = r->proj[i];
        d.vt = in.V<uint32_t>();
        d.time = in.V<uint32_t>();
        d.cat = in.V<uint8_t>();
        in.Get(d.pv, sizeof d.pv);
    }
    CameraDetail& c = r->cam;
    c.flags = in.V<uint8_t>();
    c.index = in.V<uint32_t>();
    c.count = in.V<uint32_t>();
    c.view = in.V<uint32_t>();
    in.Get(c.pos, sizeof c.pos);
    in.Get(c.target, sizeof c.target);
    in.Get(c.up, sizeof c.up);
    if (c.flags & ~kCamPresent) return false;
    return in.ok && in.i == n;
}

void EncodeDelta(const uint8_t* prev, size_t prevLen, const uint8_t* cur, size_t curLen, std::vector<uint8_t>* out) {
    auto u16 = [out](size_t v) {
        out->push_back(static_cast<uint8_t>(v));
        out->push_back(static_cast<uint8_t>(v >> 8));
    };
    if (curLen > 0xffff) return;
    if (!prev || prevLen != curLen) {
        out->push_back(0);
        u16(curLen);
        out->insert(out->end(), cur, cur + curLen);
        return;
    }
    out->push_back(1);
    u16(curLen);
    size_t i = 0;
    while (i < curLen) {
        size_t same = 0, diff = 0;
        while (i + same < curLen && same < 0xffff && cur[i + same] == prev[i + same]) ++same;
        const size_t j = i + same;
        while (j + diff < curLen && diff < 0xffff && cur[j + diff] != prev[j + diff]) ++diff;
        u16(same);
        u16(diff);
        for (size_t k = 0; k < diff; ++k) out->push_back(cur[j + k] ^ prev[j + k]);
        i = j + diff;
    }
}

size_t DecodeDelta(const uint8_t* prev, size_t prevLen, const uint8_t* p, size_t n, std::vector<uint8_t>* out) {
    if (n < 3) return 0;
    const uint8_t kind = p[0];
    const size_t len = p[1] | (static_cast<size_t>(p[2]) << 8);
    size_t i = 3;
    if (kind == 0) {
        if (n - i < len) return 0;
        out->assign(p + i, p + i + len);
        return i + len;
    }
    if (kind != 1 || !prev || prevLen != len) return 0;
    out->assign(prev, prev + len);
    size_t at = 0;
    while (at < len) {
        if (n - i < 4) return 0;
        const size_t same = p[i] | (static_cast<size_t>(p[i + 1]) << 8);
        const size_t diff = p[i + 2] | (static_cast<size_t>(p[i + 3]) << 8);
        i += 4;
        if (!same && !diff) return 0;
        if (same > len - at || diff > len - at - same || n - i < diff) return 0;
        at += same;
        for (size_t k = 0; k < diff; ++k) (*out)[at + k] ^= p[i + k];
        at += diff;
        i += diff;
    }
    return i;
}

uint8_t Recompute(const DetailRec& r, uint64_t c[kEngineComps]) {
    const uint32_t t = r.tick * kTickMs;
    c[kTimeRng] = FnvV(r.rng, FnvV(t));
    c[kTurn] = FnvV(r.activeWorm, FnvV(r.curTeam));
    uint64_t hw = kFnvBasis;
    for (int i = 0; i < r.wormCount && i < 16; ++i) {
        const WormDetail& w = r.worms[i];
        const int slot = w.slot & 0x7f;
        hw = Fnv(w.bytes, (w.slot & kSlotUnreadable) ? 0 : kWormBytes, FnvV(slot, hw));
    }
    c[kWorms] = hw;
    uint64_t hp = kFnvBasis;
    for (int i = 0; i < r.projCount && i < 64; ++i) {
        float pv[6];
        memcpy(pv, r.proj[i].pv, sizeof pv);
        hp = Fnv(pv, sizeof pv, FnvV(r.proj[i].vt, hp));
    }
    c[kProjectiles] = hp;
    uint64_t ht = kFnvBasis;
    for (auto& tm : r.teams) {
        if (tm.slot == kNoTeam) continue;
        const int i = tm.slot;
        ht = FnvV(tm.score, FnvV(tm.active, FnvV(i, ht)));
    }
    c[kTeams] = ht;
    return (1u << kTimeRng) | (1u << kTurn) | (1u << kWorms) | (1u << kProjectiles) | (1u << kTeams);
}

std::string ToJson(const DetailRec& r) {
    std::string s = "{\"tick\":" + std::to_string(r.tick) + ",\"rng\":" + Hex(r.rng) + ",\"rng2\":" + Hex(r.rng2) +
                    ",\"curTeam\":" + std::to_string(r.curTeam) + ",\"activeWorm\":" + std::to_string(r.activeWorm) +
                    ",\"worms\":[";
    for (int i = 0; i < r.wormCount && i < 16; ++i) {
        const WormDetail& w = r.worms[i];
        if (i) s += ",";
        s += "{\"slot\":" + std::to_string(w.slot & 0x7f);
        if (w.slot & kSlotUnreadable) {
            s += ",\"unreadable\":true}";
            continue;
        }
        for (const Field& f : kWorm) {
            s += ",\"";
            s += f.name;
            s += "\":";
            if (f.kind == F3)
                s += "[" + Num(F(w.bytes + f.pos)) + "," + Num(F(w.bytes + f.pos + 4)) + "," +
                     Num(F(w.bytes + f.pos + 8)) + "]";
            else
                s += Scalar(w.bytes + f.pos, f.kind);
        }
        s += "}";
    }
    s += "],\"teams\":[";
    bool first = true;
    for (auto& t : r.teams) {
        if (t.slot == kNoTeam) continue;
        if (!first) s += ",";
        first = false;
        s += "{\"slot\":" + std::to_string(t.slot) + ",\"active\":" + std::to_string(t.active) +
             ",\"score\":" + std::to_string(static_cast<int32_t>(t.score)) + "}";
    }
    s += "],\"projectiles\":[";
    for (int i = 0; i < r.projCount && i < 64; ++i) {
        const ProjDetail& p = r.proj[i];
        if (i) s += ",";
        s += "{\"vt\":" + Hex(p.vt) + ",\"time\":" + std::to_string(p.time) + ",\"cat\":" + std::to_string(p.cat) +
             ",\"pos\":" + Vec(p.pv) + ",\"vel\":" + Vec(p.pv + 3) + "}";
    }
    const CameraDetail& c = r.cam;
    s += "],\"camera\":{\"present\":";
    if (c.flags & kCamPresent) {
        char h[24];
        snprintf(h, sizeof h, "\"%016llx\"", static_cast<unsigned long long>(CameraHash(c)));
        s += "true,\"index\":" + std::to_string(c.index) + ",\"count\":" + std::to_string(c.count) +
             ",\"view\":" + std::to_string(c.view) + ",\"pos\":" + Vec(c.pos) + ",\"target\":" + Vec(c.target) +
             ",\"up\":" + Vec(c.up) + ",\"hash\":" + h;
    } else {
        s += "false";
    }
    s += "}}";
    return s;
}

std::string Diff(const DetailRec& a, const DetailRec& b) {
    std::string s;
    auto line = [&s](const std::string& path, const std::string& va, const std::string& vb) {
        if (va != vb) s += path + " " + Unquote(va) + " -> " + Unquote(vb) + "\n";
    };
    line("tick", std::to_string(a.tick), std::to_string(b.tick));
    line("rng", Hex(a.rng), Hex(b.rng));
    line("rng2", Hex(a.rng2), Hex(b.rng2));
    line("curTeam", std::to_string(a.curTeam), std::to_string(b.curTeam));
    line("activeWorm", std::to_string(a.activeWorm), std::to_string(b.activeWorm));
    for (uint8_t slot = 0; slot < 16; ++slot) {
        const WormDetail* wa = FindWorm(a, slot);
        const WormDetail* wb = FindWorm(b, slot);
        const std::string p = "worm[" + std::to_string(slot) + "]";
        if (!wa && !wb) continue;
        if (!wa || !wb || ((wa->slot ^ wb->slot) & kSlotUnreadable)) {
            line(p, wa ? ((wa->slot & kSlotUnreadable) ? "unreadable" : "present") : "absent",
                 wb ? ((wb->slot & kSlotUnreadable) ? "unreadable" : "present") : "absent");
            continue;
        }
        if (wa->slot & kSlotUnreadable) continue;
        for (const Field& f : kWorm) {
            if (!memcmp(wa->bytes + f.pos, wb->bytes + f.pos, Width(f.kind))) continue;
            if (f.kind == F3) {
                for (int k = 0; k < 3; ++k)
                    if (memcmp(wa->bytes + f.pos + 4 * k, wb->bytes + f.pos + 4 * k, 4))
                        line(p + "." + f.name + kAxis[k], Num(F(wa->bytes + f.pos + 4 * k)),
                             Num(F(wb->bytes + f.pos + 4 * k)));
            } else {
                line(p + "." + f.name, Scalar(wa->bytes + f.pos, f.kind), Scalar(wb->bytes + f.pos, f.kind));
            }
        }
    }
    for (uint8_t slot = 0; slot < 4; ++slot) {
        const TeamDetail *ta = nullptr, *tb = nullptr;
        for (auto& t : a.teams)
            if (t.slot == slot) ta = &t;
        for (auto& t : b.teams)
            if (t.slot == slot) tb = &t;
        const std::string p = "team[" + std::to_string(slot) + "]";
        if (!ta && !tb) continue;
        if (!ta || !tb) {
            line(p, ta ? "present" : "absent", tb ? "present" : "absent");
            continue;
        }
        line(p + ".active", std::to_string(ta->active), std::to_string(tb->active));
        line(p + ".score", std::to_string(static_cast<int32_t>(ta->score)), std::to_string(static_cast<int32_t>(tb->score)));
    }
    line("projectiles", std::to_string(a.projCount), std::to_string(b.projCount));
    const int pc = a.projCount < b.projCount ? a.projCount : b.projCount;
    for (int i = 0; i < pc && i < 64; ++i) {
        const ProjDetail &pa = a.proj[i], &pb = b.proj[i];
        const std::string p = "proj[" + std::to_string(i) + "]";
        line(p + ".vt", Hex(pa.vt), Hex(pb.vt));
        line(p + ".time", std::to_string(pa.time), std::to_string(pb.time));
        for (int k = 0; k < 6; ++k)
            if (pa.pv[k] != pb.pv[k])
                line(p + (k < 3 ? ".pos" : ".vel") + kAxis[k % 3], Num(Fb(pa.pv[k])), Num(Fb(pb.pv[k])));
    }
    const CameraDetail &ca = a.cam, &cb = b.cam;
    const bool ina = ca.flags & kCamPresent, inb = cb.flags & kCamPresent;
    line("camera", ina ? "present" : "absent", inb ? "present" : "absent");
    if (ina && inb) {
        line("camera.index", std::to_string(ca.index), std::to_string(cb.index));
        line("camera.count", std::to_string(ca.count), std::to_string(cb.count));
        line("camera.view", std::to_string(ca.view), std::to_string(cb.view));
        const struct {
            const char* name;
            const uint32_t *va, *vb;
        } vecs[] = {{".pos", ca.pos, cb.pos}, {".target", ca.target, cb.target}, {".up", ca.up, cb.up}};
        for (const auto& v : vecs)
            for (int k = 0; k < 3; ++k)
                if (v.va[k] != v.vb[k]) line(std::string("camera") + v.name + kAxis[k], Num(Fb(v.va[k])), Num(Fb(v.vb[k])));
    }
    return s;
}
}  // namespace melange::wormsign::detail
