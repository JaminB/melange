// Event bus (component B): message classes (vtable -> name) and the built-in typed payload decoders.
// Vtables and layouts: docs/m0-design.md §1.2 (re/out/vtables.tsv; ExplosionMessage from ctor 0x518ce0).
// Offsets are from the start of the message object (+0 vtable, +4 u16 id, +8 payload).
#include <windows.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string.h>
#include <unordered_map>

#include "core/bus_internal.h"
#include "core/log.h"

namespace wf::bus {
namespace {
struct Entry {
    const char* name;  // static lifetime (literal or leaked copy)
    Decoder fn;
};

std::shared_mutex& Lock() {
    static std::shared_mutex m;
    return m;
}
std::unordered_map<uintptr_t, Entry>& Map() {
    static std::unordered_map<uintptr_t, Entry> m;
    return m;
}

// ---- guarded reads
// Copies a NUL-terminated engine string (at most cap-1 chars). Stops at the first unreadable byte.
size_t ReadCString(uintptr_t p, char* buf, size_t cap) {
    size_t n = 0;
    if (!p || cap == 0) return 0;
    __try {
        const char* s = reinterpret_cast<const char*>(p);
        while (n + 1 < cap && s[n]) {
            buf[n] = s[n];
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    buf[n] = '\0';
    return n;
}

void StrField(const MessageView& m, uint32_t off, const char* key, JsonOut& o) {
    uint32_t p = 0;
    if (!m.Get(off, p)) return;
    if (!p) {
        o.Hex(key, 0);  // null pointer: say so rather than print ""
        return;
    }
    char buf[128];
    size_t n = ReadCString(p, buf, sizeof(buf));
    o.Str(key, std::string_view(buf, n));
}

void Vec3Field(const MessageView& m, uint32_t off, const char* key, JsonOut& o) {
    float v[3];
    if (m.Read(off, v, sizeof(v))) o.Vec3(key, v);
}

void FloatField(const MessageView& m, uint32_t off, const char* key, JsonOut& o) {
    float f;
    if (m.Get(off, f)) o.Float(key, f);
}

// ---- built-in decoders
void DecMessage(const MessageView&, JsonOut&) {}  // bare Message: no payload

void DecInt(const MessageView& m, JsonOut& o) {
    int32_t v;
    if (m.Get(8, v)) o.Int("value", v);
}
void DecUint(const MessageView& m, JsonOut& o) {
    uint32_t v;
    if (m.Get(8, v)) o.Uint("value", v);
}
void DecFloat(const MessageView& m, JsonOut& o) { FloatField(m, 8, "value", o); }
void DecTwoInt(const MessageView& m, JsonOut& o) {
    int32_t v;
    if (m.Get(8, v)) o.Int("a", v);
    if (m.Get(0xc, v)) o.Int("b", v);
}
void DecTwoFloat(const MessageView& m, JsonOut& o) {
    FloatField(m, 8, "a", o);
    FloatField(m, 0xc, "b", o);
}
void DecString(const MessageView& m, JsonOut& o) { StrField(m, 8, "value", o); }
void DecTwoString(const MessageView& m, JsonOut& o) {
    StrField(m, 8, "a", o);
    StrField(m, 0xc, "b", o);
}
void DecVector(const MessageView& m, JsonOut& o) { Vec3Field(m, 8, "v", o); }
void DecVectorUint(const MessageView& m, JsonOut& o) {
    Vec3Field(m, 8, "v", o);
    uint32_t u;
    if (m.Get(0x14, u)) o.Uint("u", u);
}
void DecPayloadEvent(const MessageView& m, JsonOut& o) {
    uint32_t id;
    if (m.Get(8, id)) o.Uint("payloadId", id);
    FloatField(m, 0xc, "time", o);
    Vec3Field(m, 0x10, "pos", o);
    Vec3Field(m, 0x1c, "vel", o);
}
void DecExplosion(const MessageView& m, JsonOut& o) {
    Vec3Field(m, 0x08, "damageEpicentre", o);
    Vec3Field(m, 0x14, "impulseEpicentre", o);
    Vec3Field(m, 0x20, "dir", o);
    FloatField(m, 0x2c, "wormDamage", o);
    FloatField(m, 0x30, "impulse", o);
    FloatField(m, 0x34, "wormDamageRadius", o);
    FloatField(m, 0x38, "landDamageRadius", o);
    FloatField(m, 0x3c, "impulseRadius", o);
}

struct Builtin {
    uintptr_t vtable;
    const char* name;
    Decoder fn;  // nullptr: class known by name only (payload not decoded)
};
constexpr Builtin kBuiltins[] = {
    {0x81aa14, "Message", &DecMessage},
    {0x851f24, "TaskIDMessage", &DecUint},
    {0x8850d4, "IntMessage", &DecInt},
    {0x8850dc, "TwoIntMessage", &DecTwoInt},
    {0x8850e4, "UintMessage", &DecUint},
    {0x8863c0, "FloatMessage", &DecFloat},
    {0x8863c8, "TwoFloatMessage", &DecTwoFloat},
    {0x884fac, "StringMessage", &DecString},
    {0x884fb4, "TwoStringMessage", &DecTwoString},
    {0x886298, "VectorMessage", &DecVector},
    {0x8862a0, "VectorUintMessage", &DecVectorUint},
    {0x85be94, "PayloadEventMessage", &DecPayloadEvent},
    {0x854184, "ExplosionMessage", &DecExplosion},
    {0x81c444, "LandNewShapeMessage", nullptr},
    {0x82c3e4, "ActingTriggerMsg", nullptr},
    {0x830eb0, "WormCommandMessage", nullptr},
    {0x854108, "DamageImpulseMessage", nullptr},
    {0x854978, "CameraSceneMessage", nullptr},
    {0x861f00, "CreateGravestoneMessage", nullptr},
};

std::once_flag g_builtinsOnce;

bool CallDecoder(Decoder fn, const MessageView& m, JsonOut& out, DWORD* code) {
    __try {
        fn(m, out);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

namespace detail {
void RegisterBuiltinDecoders() {
    std::call_once(g_builtinsOnce, [] {
        std::unique_lock lk(Lock());
        for (const auto& b : kBuiltins) Map().try_emplace(b.vtable, Entry{b.name, b.fn});
    });
}

const char* ClassNameOf(uintptr_t vtable) {
    RegisterBuiltinDecoders();
    std::shared_lock lk(Lock());
    auto it = Map().find(vtable);
    return it == Map().end() ? "?" : it->second.name;
}
}  // namespace detail

bool RegisterDecoder(uintptr_t vtable, const char* className, Decoder fn) {
    if (!vtable) return false;
    detail::RegisterBuiltinDecoders();
    std::unique_lock lk(Lock());
    auto it = Map().find(vtable);
    const char* name = nullptr;
    if (className && *className) {
        name = (it != Map().end() && strcmp(it->second.name, className) == 0) ? it->second.name : _strdup(className);
    } else {
        name = it != Map().end() ? it->second.name : "?";
    }
    Map()[vtable] = Entry{name, fn};
    return true;
}

bool Decode(const MessageView& m, JsonOut& out) {
    Decoder fn = nullptr;
    {
        detail::RegisterBuiltinDecoders();
        std::shared_lock lk(Lock());
        auto it = Map().find(m.vtable);
        if (it != Map().end()) fn = it->second.fn;
    }
    if (!fn) return false;
    DWORD code = 0;
    if (CallDecoder(fn, m, out, &code)) return true;
    static std::atomic<int> s_logged{0};
    if (s_logged.fetch_add(1) < 5)
        WF_WARN("[bus] decoder for %s (vtable %08x) faulted with %08lx; output may be partial", m.className,
                static_cast<unsigned>(m.vtable), code);
    return false;
}
}  // namespace wf::bus
