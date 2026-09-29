#include "weapons/contrib.h"

#include <windows.h>

#include <algorithm>

#include "core/log.h"
#include "core/mem.h"
#include "lua/sim/tweak.h"
#include "melange/sim.h"
#include "melange/weapons.h"
#include "weapons/behaviour.h"
#include "weapons/engine.h"
#include "weapons/manifest.h"

namespace melange::weapons::contrib {
namespace {
constexpr int kMaxReads = 128;
constexpr size_t kMaxText = 64;

struct ReadOp {
    uint8_t clone, size;  // size 0: an XString field, hashed by content
    uint32_t offset;
};
struct Plan {
    uintptr_t containers[kMaxClones] = {};
    uint32_t serial = 0;
    size_t touched = static_cast<size_t>(-1);
    ReadOp ops[kMaxReads] = {};
    int n = 0;
};
Plan g_plan;
int g_handle = 0;
const char* g_vanilla[kMaxClones] = {};

size_t Size(FieldType t) {
    switch (t) {
        case FieldType::F32:
        case FieldType::I32:
        case FieldType::U32: return 4;
        case FieldType::U16: return 2;
        case FieldType::U8:
        case FieldType::Bool: return 1;
        default: return 0;
    }
}

void Add(Plan& p, int clone, uint32_t offset, FieldType t) {
    if (p.n >= kMaxReads || (t != FieldType::String && !Size(t))) return;
    for (int i = 0; i < p.n; ++i)
        if (p.ops[i].clone == clone && p.ops[i].offset == offset) return;
    p.ops[p.n++] = {static_cast<uint8_t>(clone), static_cast<uint8_t>(Size(t)), offset};
}

bool Stale(const CloneInfo* c, int n) {
    if (sim::MatchSerial() != g_plan.serial || tweak::Instance().TouchedFields().size() != g_plan.touched) return true;
    for (int i = 0; i < kMaxClones; ++i)
        if (g_plan.containers[i] != (i < n ? c[i].container : 0)) return true;
    return false;
}

void Build(const CloneInfo* c, int n) {
    Plan p;
    const auto& touched = tweak::Instance().TouchedFields();
    p.serial = sim::MatchSerial();
    p.touched = touched.size();
    for (int i = 0; i < n; ++i) {
        p.containers[i] = c[i].container;
        if (!c[i].container) continue;
        for (auto& d : manifest::Frozen()) {
            if (d.k != c[i].k) continue;
            for (auto& s : d.set) {
                uint32_t off = 0;
                const FieldType t = Field(c[i].container, s.field.c_str(), &off);
                if (t != FieldType::None) Add(p, i, off, t);
            }
        }
        for (auto& t : touched)
            if (t.container == c[i].container) Add(p, i, t.offset, t.type);
    }
    g_plan = p;
}

size_t ReadText(uintptr_t p, char* out, size_t max) {
    size_t n = 0;
    __try {
        const char* s = reinterpret_cast<const char*>(p);
        while (n < max && s[n]) {
            out[n] = s[n];
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void Contrib(wormsign::Hasher& h, uint32_t, void*) { Feed(h); }
}  // namespace

void Feed(wormsign::Hasher& h) {
    CloneInfo c[kMaxClones];
    const int n = std::clamp(Declared(c, kMaxClones), 0, kMaxClones);
    const uint8_t live = Live() ? 1 : 0;
    const int32_t active = ActiveClone();
    h.Val(live);
    h.Val(active);
    for (int i = 0; i < n; ++i) {
        const uint8_t swapped = engine::EnumName(c[i].base) != g_vanilla[i] ? 1 : 0;
        h.Val(c[i].base);
        h.Val(swapped);
    }
    const auto count = behaviour::GetCounters();
    h.Val(count.fires);
    h.Val(count.explosions);
    h.Val(count.extras);
    if (!live) return;
    if (Stale(c, n)) Build(c, n);
    for (int i = 0; i < g_plan.n; ++i) {
        const ReadOp& op = g_plan.ops[i];
        const uintptr_t base = op.clone < n ? c[op.clone].container : 0;
        if (!base) continue;
        if (op.size) {
            uint32_t bits = 0;
            mem::SafeRead(base + op.offset, &bits, op.size);
            h.Bytes(&bits, op.size);
            continue;
        }
        uintptr_t p = 0;
        char text[kMaxText];
        const uint32_t len = mem::SafeRead(base + op.offset, &p, sizeof p) && p
                                 ? static_cast<uint32_t>(ReadText(p, text, kMaxText))
                                 : 0;
        h.Val(len);
        h.Bytes(text, len);
    }
}

bool Register() {
    if (g_handle) return true;
    CloneInfo c[kMaxClones];
    const int n = std::clamp(Declared(c, kMaxClones), 0, kMaxClones);
    if (!n) return false;
    for (int i = 0; i < n; ++i) g_vanilla[i] = engine::EnumName(c[i].base);
    g_plan = Plan{};
    g_handle = wormsign::AddContributor(kName, &Contrib, nullptr);
    if (g_handle <= 0) {
        g_handle = 0;
        LOG_ERROR("[weapons] the %s Wormsign contributor could not be added", kName);
        return false;
    }
    LOG_INFO("[weapons] Wormsign contributor %s added (%d clone(s))", kName, n);
    return true;
}

void Unregister() {
    if (g_handle) wormsign::RemoveContributor(g_handle);
    g_handle = 0;
}
}  // namespace melange::weapons::contrib
