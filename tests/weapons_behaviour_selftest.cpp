// Offline self-test for the clone behaviour core (observers, the explosion window and its limits, counters) and the
// melange.weapons Wormsign contributor. The registry, the field walk, the name slots and Wormsign are fakes below.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

#include "lua/sim/tweak.h"
#include "melange/sim.h"
#include "melange/weapons.h"
#include "melange/wormsign.h"
#include "weapons/behaviour.h"
#include "weapons/contrib.h"
#include "weapons/engine.h"
#include "weapons/manifest.h"

namespace mw = melange::weapons;
namespace bh = melange::weapons::behaviour;
using mw::Event;
using mw::FieldType;
using mw::QueueResult;

namespace {
int g_pass = 0, g_fail = 0;
void Expect(bool ok, const std::string& what) {
    ok ? ++g_pass : ++g_fail;
    if (!ok) std::printf("FAIL: %s\n", what.c_str());
}

struct alignas(8) Fake {
    unsigned char bytes[64];
};
Fake g_clone;
std::deque<std::string> g_strings;
uintptr_t Addr(Fake& f) { return reinterpret_cast<uintptr_t>(f.bytes); }
void SetText(uintptr_t field, const char* s) {
    g_strings.emplace_back(s);
    const uintptr_t p = reinterpret_cast<uintptr_t>(g_strings.back().c_str());
    std::memcpy(reinterpret_cast<void*>(field), &p, sizeof p);
}

int g_declared = 0, g_active = -1;
bool g_live = false;
uint32_t g_serial = 1;
const char g_vanillaName[] = "kWeaponBazooka";
const char g_cloneName[] = "kWeaponMega";
const char* g_slot = g_vanillaName;

struct FakeField {
    const char* name;
    FieldType type;
    uint32_t offset;
};
constexpr FakeField kFields[] = {
    {"WormDamageMagnitude", FieldType::F32, 0x00},
    {"LifeTime", FieldType::I32, 0x04},
    {"PayloadGraphicsResourceID", FieldType::String, 0x10},
};

melange::wormsign::ContribFn g_fn = nullptr;
std::string g_contribName;
int g_removed = 0;

class Rec final : public melange::wormsign::Hasher {
public:
    std::string bytes;
    void Bytes(const void* p, size_t n) override { bytes.append(static_cast<const char*>(p), n); }
};

std::string Hash() {
    Rec r;
    if (g_fn) g_fn(r, 1, nullptr);
    return r.bytes;
}

std::vector<std::string> g_seen;
void Obs(const mw::EventArgs& a, void* user) {
    g_seen.push_back(std::string(static_cast<const char*>(user)) + ":" + std::to_string(static_cast<int>(a.ev)));
}
int g_victim = 0;
void Remover(const mw::EventArgs&, void*) { mw::RemoveOn(g_victim); }
void Thrower(const mw::EventArgs&, void*) { throw std::runtime_error("x"); }
}  // namespace

namespace melange::weapons {
int Declared(CloneInfo* out, int max) {
    for (int i = 0; i < g_declared && i < max; ++i) {
        out[i] = {};
        out[i].k = static_cast<uint16_t>(i);
        out[i].vid = kVidBase + i;
        out[i].base = 1;
        std::snprintf(out[i].name, sizeof out[i].name, "%s", g_cloneName);
        out[i].live = g_live;
        out[i].container = g_live ? Addr(g_clone) : 0;
    }
    return g_declared;
}
bool Live() { return g_live; }
int ActiveClone() { return g_active; }
uintptr_t Container(const char* name) { return name && std::strcmp(name, g_cloneName) == 0 ? Addr(g_clone) : 0; }
FieldType Field(uintptr_t container, const char* field, uint32_t* offset) {
    if (!container || !field) return FieldType::None;
    for (auto& f : kFields)
        if (std::strcmp(f.name, field) == 0) {
            if (offset) *offset = f.offset;
            return f.type;
        }
    return FieldType::None;
}
namespace engine {
const char* EnumName(int id) { return id == 1 ? g_slot : nullptr; }
std::string XStringValue(uintptr_t field) {
    uintptr_t p = 0;
    std::memcpy(&p, reinterpret_cast<void*>(field), sizeof p);
    return p ? reinterpret_cast<const char*>(p) : "";
}
bool AssignXString(uintptr_t field, const char* s) {
    SetText(field, s);
    return true;
}
}  // namespace engine
}  // namespace melange::weapons

namespace melange::sim {
uint32_t MatchSerial() { return g_serial; }
}  // namespace melange::sim

namespace melange::wormsign {
int AddContributor(const char* name, ContribFn fn, void*, const ContribOptions&) {
    g_contribName = name;
    g_fn = fn;
    return 7;
}
void RemoveContributor(int handle) {
    if (handle == 7) ++g_removed;
    g_fn = nullptr;
}
}  // namespace melange::wormsign

namespace melange::simbridge {
bool AddSimFunction(const char*, lua50::CFunction) { return true; }
bool InTopLevelChunk() { return true; }
}  // namespace melange::simbridge

void TestObservers() {
    static char a[] = "a", b[] = "b", c[] = "c";
    const int ha = mw::On(&Obs, a, 5);
    const int hb = mw::On(&Obs, b, 0);
    const int hc = mw::On(&Obs, c, 5);
    Expect(ha && hb && hc && mw::On(nullptr, nullptr) == 0, "On() returns handles, refuses a null function");
    mw::EventArgs e{};
    e.ev = Event::Impact;
    bh::Raise(e);
    Expect(g_seen == std::vector<std::string>({"b:2", "a:2", "c:2"}), "observers by order, then registration");
    g_seen.clear();
    g_victim = hc;
    const int hr = mw::On(&Remover, nullptr, 1);
    const int ht = mw::On(&Thrower, nullptr, 2);
    bh::Raise(e);
    Expect(g_seen == std::vector<std::string>({"b:2", "a:2"}), "an observer removed during a raise is skipped; a throw "
                                                               "does not stop the others");
    g_seen.clear();
    for (int h : {ha, hb, hr, ht}) mw::RemoveOn(h);
    bh::Raise(e);
    Expect(g_seen.empty(), "RemoveOn() removes");
}

void TestQueue() {
    const float d[3] = {90, 0, 0};
    Expect(mw::QueueExplosion(d) == QueueResult::NotInExplosion && !bh::InExplosion(), "no window: NotInExplosion");
    bh::SetExtraLimit(100);
    Expect(bh::ExtraLimit() == bh::kMaxExtraCap, "the limit is capped at 8");
    bh::SetExtraLimit(-1);
    Expect(bh::ExtraLimit() == 0, "the limit is at least 0");
    const float origin[3] = {10, 20, 30};
    bh::OpenExplosion(origin);
    Expect(bh::InExplosion() && mw::QueueExplosion(d) == QueueResult::Full, "limit 0: Full");
    float out[8][3];
    Expect(bh::CloseExplosion(out, 8) == 0 && !bh::InExplosion(), "nothing queued");

    bh::SetExtraLimit(3);
    bh::OpenExplosion(origin);
    const float far[3] = {0, 2000.5f, 0}, edge[3] = {-2000, 2000, 0}, nan[3] = {NAN, 0, 0}, inf[3] = {0, 0, INFINITY};
    Expect(mw::QueueExplosion(far) == QueueResult::OutOfRange, "|d| > 2000: OutOfRange");
    Expect(mw::QueueExplosion(nan) == QueueResult::OutOfRange && mw::QueueExplosion(inf) == QueueResult::OutOfRange,
           "non-finite offsets: OutOfRange");
    Expect(mw::QueueExplosion(nullptr) == QueueResult::OutOfRange, "null offsets: OutOfRange");
    Expect(mw::QueueExplosion(d) == QueueResult::Ok && mw::QueueExplosion(edge) == QueueResult::Ok &&
               mw::QueueExplosion(d) == QueueResult::Ok,
           "three extras at the limit");
    Expect(mw::QueueExplosion(d) == QueueResult::Full, "the fourth: Full");
    Expect(mw::QueueExplosion(far) == QueueResult::OutOfRange, "range is checked before the limit");
    const int n = bh::CloseExplosion(out, 8);
    Expect(n == 3 && out[0][0] == 100 && out[0][1] == 20 && out[1][0] == -1990 && out[1][1] == 2020 && out[2][2] == 30,
           "extras are origin + d, in call order");
    Expect(mw::QueueExplosion(d) == QueueResult::NotInExplosion, "closed after the event");
    bh::OpenExplosion(origin);
    mw::QueueExplosion(d);
    mw::QueueExplosion(d);
    Expect(bh::CloseExplosion(out, 1) == 1, "CloseExplosion honours max");
    bh::OpenExplosion(origin);
    Expect(bh::CloseExplosion(out, 8) == 0, "a new window starts empty");

    bh::ResetCounters();
    bh::CountEvent(Event::Fire);
    bh::CountEvent(Event::Tick);
    bh::CountEvent(Event::Tick);
    bh::CountEvent(Event::Impact);
    bh::CountEvent(Event::Explosion);
    bh::CountExtra();
    const auto c = bh::GetCounters();
    Expect(c.fires == 1 && c.ticks == 2 && c.impacts == 1 && c.explosions == 1 && c.extras == 1, "counters");
    bh::ResetCounters();
    Expect(bh::GetCounters().fires == 0, "counters reset");
}

void TestContributor() {
    namespace ct = melange::weapons::contrib;
    std::memset(g_clone.bytes, 0, sizeof g_clone.bytes);
    const float dmg = 120;
    std::memcpy(g_clone.bytes, &dmg, 4);
    SetText(Addr(g_clone) + 0x10, "Bazooka.Payload");

    g_declared = 0;
    Expect(!ct::Register() && !g_fn, "no contributor without declared clones");

    mw::manifest::CloneDecl d;
    d.mod = "m";
    d.name = g_cloneName;
    d.base = "kWeaponBazooka";
    d.baseId = 1;
    d.cell = 29;
    mw::manifest::SetValue s1, s2;
    s1.field = "WormDamageMagnitude";
    s1.type = FieldType::F32;
    s2.field = "PayloadGraphicsResourceID";
    s2.type = FieldType::String;
    d.set = {s1, s2};
    mw::manifest::Freeze({d});
    g_declared = 1;
    Expect(ct::Register() && g_fn && g_contribName == ct::kName, "registered as melange.weapons");
    Expect(ct::Register(), "a second Register() is a no-op");

    const std::string off = Hash();
    Expect(off.size() == 1 + 4 + 4 + 1 + 12, "not live: state, slot and counters only (" + std::to_string(off.size()) + ")");
    Expect(Hash() == off, "deterministic");
    g_live = true;
    const std::string on = Hash();
    Expect(on != off && on.size() == off.size() + 4 + 4 + 15, "live: the set fields are hashed (" +
                                                                  std::to_string(on.size()) + ")");
    g_clone.bytes[4] = 9;
    Expect(Hash() == on, "a field no one set is not hashed");
    const float poked = 121;
    std::memcpy(g_clone.bytes, &poked, 4);
    const std::string pokedHash = Hash();
    Expect(pokedHash != on, "a poked set field changes the hash");
    std::memcpy(g_clone.bytes, &dmg, 4);
    Expect(Hash() == on, "and restoring it restores the hash");
    SetText(Addr(g_clone) + 0x10, "Bazooka.Payload");
    Expect(Hash() == on, "strings are hashed by content, not address");
    SetText(Addr(g_clone) + 0x10, "Grenade.Payload");
    Expect(Hash() != on, "a changed string changes the hash");
    SetText(Addr(g_clone) + 0x10, "Bazooka.Payload");

    melange::tweak::Value v;
    v.type = FieldType::F32;
    v.num = 7;
    Expect(melange::tweak::Set(g_cloneName, "LifeTime", v) == melange::tweak::TweakError::Ok, "a Lua set on the clone");
    const std::string lua = Hash();
    Expect(lua.size() == on.size() + 4, "fields set from Lua are hashed too");
    g_clone.bytes[4] = 8;
    Expect(Hash() != lua, "and a later poke of one is seen");
    g_clone.bytes[4] = 7;

    g_slot = g_cloneName;
    Expect(Hash() != lua, "the name slot swap is hashed");
    g_slot = g_vanillaName;
    g_active = 0;
    Expect(Hash() != lua, "ActiveClone() is hashed");
    g_active = -1;
    bh::CountExtra();
    Expect(Hash() != lua, "the counters are hashed");
    bh::ResetCounters();
    Expect(Hash() == lua, "back to the same state, the same bytes");

    melange::tweak::Instance().RestoreAll([](const char* w, void*) { return mw::Container(w); }, nullptr);
    ++g_serial;
    Expect(Hash() == on, "a new match drops the Lua-set fields from the plan");
    g_live = false;
    Expect(Hash() == off, "not live again");
    ct::Unregister();
    Expect(g_removed == 1 && !g_fn, "Unregister() removes it");
}

int main() {
    TestObservers();
    TestQueue();
    TestContributor();
    std::printf("weapons_behaviour_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
