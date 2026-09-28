#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "game/state/gamestate_json.h"
#include "tools/json_read.h"

// Parameters and filters of the state/entities channels and methods (pure; covered by gamestate_selftest).
namespace melange::oasis::stateparams {
struct Filter { uint32_t hz; uint32_t kinds; };

inline uint32_t Kinds(const json::Value* v) {
    if (!v || !v->IsArray()) return gamestate::wire::kAllKinds;
    uint32_t m = 0;
    for (const auto& k : v->items) {
        gamestate::EntityKind kind;
        if (k.IsString() && gamestate::wire::KindFromName(k.string, &kind)) m |= gamestate::wire::KindBit(kind);
    }
    return m;
}

// {hz, kinds}; hz is clamped to 1..maxHz, anything malformed falls back to the defaults.
inline Filter ParseFilter(std::string_view text, uint32_t defHz, uint32_t maxHz) {
    Filter f{defHz, gamestate::wire::kAllKinds};
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e, 4096) || !v.IsObject()) return f;
    if (const json::Value* hz = v.Get("hz"); hz && hz->IsNumber())
        f.hz = hz->number < 1 ? 1 : hz->number > maxHz ? maxHz : static_cast<uint32_t>(hz->number);
    f.kinds = Kinds(v.Get("kinds"));
    return f;
}

struct Inspect {
    bool byHandle = false;
    uint32_t handle = 0;
    uintptr_t addr = 0;
    uint32_t len = 256;
    std::string error;  // non-empty: -32602
};

inline bool Address(const json::Value* v, uintptr_t* out) {
    if (!v) return false;
    if (v->IsInteger() && v->number > 0 && v->number <= 4294967295.0) {
        *out = static_cast<uintptr_t>(v->number);
        return true;
    }
    if (!v->IsString()) return false;
    const std::string& s = v->string;
    if (s.size() < 3 || s.size() > 10 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    char* end = nullptr;
    const unsigned long x = strtoul(s.c_str() + 2, &end, 16);
    if (!end || *end || !x) return false;
    *out = static_cast<uintptr_t>(x);
    return true;
}

// {handle, len?} or {addr, len?}: addr a positive integer or "0x..." string, len 1..4096 (default 256).
inline Inspect ParseInspect(std::string_view text) {
    Inspect r;
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e, 4096) || !v.IsObject()) return r.error = "params must be an object", r;
    const json::Value* h = v.Get("handle");
    const json::Value* a = v.Get("addr");
    if (!!h == !!a) return r.error = "give either handle or addr", r;
    if (h) {
        if (!h->IsInteger() || h->number < 0 || h->number > 4294967295.0) return r.error = "handle must be an unsigned integer", r;
        r.byHandle = true;
        r.handle = static_cast<uint32_t>(h->number);
    } else if (!Address(a, &r.addr)) {
        return r.error = "addr must be a positive integer or a \"0x...\" string", r;
    }
    if (const json::Value* l = v.Get("len")) {
        if (!l->IsInteger() || l->number < 1 || l->number > 4096) return r.error = "len must be 1..4096", r;
        r.len = static_cast<uint32_t>(l->number);
    }
    if (!r.byHandle && r.addr + r.len < r.addr) return r.error = "range wraps around", r;
    return r;
}

// {prefix?}: at most 63 characters. False: bad params.
inline bool ParsePrefix(std::string_view text, std::string* prefix) {
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e, 4096) || !v.IsObject()) return false;
    const json::Value* p = v.Get("prefix");
    if (!p || p->IsNull()) return prefix->clear(), true;
    if (!p->IsString() || p->string.size() > 63) return false;
    *prefix = p->string;
    return true;
}

// Hex of `n` bytes; `ok[i] == 0` renders "??".
inline std::string Hex(const uint8_t* bytes, const uint8_t* ok, uint32_t n) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (uint32_t i = 0; i < n; ++i) {
        if (!ok[i]) {
            s += "??";
            continue;
        }
        s += kDigits[bytes[i] >> 4];
        s += kDigits[bytes[i] & 15];
    }
    return s;
}
}  // namespace melange::oasis::stateparams
