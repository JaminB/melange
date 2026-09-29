#include <algorithm>
#include <cstring>
#include <initializer_list>

#include "melange/gamestate.h"
#include "tools/json_read.h"
#include "wormsign/hash_engine.h"
#include "wormsign/setup.h"

namespace melange::wormsign::setup {
namespace {
std::string VarString(const char* name) {
    gamestate::Var v;
    if (!gamestate::Var1(name, &v) || v.type != gamestate::VarType::String) return "";
    json::Value j;
    json::Error e;
    return json::Parse(v.value, &j, &e) && j.IsString() ? j.string : "";
}

uintptr_t VarObject(const char* name) {
    gamestate::Var v;
    if (!gamestate::Var1(name, &v) || v.type != gamestate::VarType::Container) return 0;
    json::Value j;
    json::Error e;
    if (!json::Parse(v.value, &j, &e)) return 0;
    const json::Value* a = j.Get("addr");
    return a && a->IsNumber() ? static_cast<uintptr_t>(a->number) : 0;
}

// A NUL-terminated string of printable bytes at p, read page by page; false if p is not one.
bool CString(uintptr_t p, std::string* out) {
    out->clear();
    if (p < 0x10000) return false;
    char buf[256];
    size_t n = 0;
    while (n < sizeof buf) {
        const size_t toPage = 0x1000 - ((p + n) & 0xfff);
        const size_t chunk = (std::min)(toPage, sizeof buf - n);
        if (!gamestate::Peek(p + n, buf + n, static_cast<uint32_t>(chunk))) return false;
        for (size_t i = n; i < n + chunk; ++i) {
            const auto c = static_cast<unsigned char>(buf[i]);
            if (!c) {
                for (size_t k = 0; k < i; ++k) {
                    const auto b = static_cast<unsigned char>(buf[k]);
                    if (b < 0x80) {
                        out->push_back(static_cast<char>(b));
                    } else {
                        out->push_back(static_cast<char>(0xc0 | b >> 6));
                        out->push_back(static_cast<char>(0x80 | (b & 0x3f)));
                    }
                }
                return true;
            }
            if (c < 0x20 && c != '\t') return false;
        }
        n += chunk;
    }
    return false;
}

// Dwords of [from, to): below 0x10000 raw, a string pointer as its text, any other pointer as a marker only (it
// differs between processes). `skip` holds object references, which are left out.
uint64_t HashFields(const uint8_t* obj, uint32_t from, uint32_t to, std::initializer_list<uint32_t> skip, uint64_t h) {
    std::string s;
    for (uint32_t off = from; off + 4 <= to; off += 4) {
        bool skipped = false;
        for (uint32_t k : skip) skipped |= k == off;
        if (skipped) continue;
        uint32_t v;
        memcpy(&v, obj + off, 4);
        h = FnvV(off, h);
        if (v < 0x10000) {
            h = FnvV(v, h);
        } else if (CString(v, &s)) {
            h = Fnv(s.data(), s.size(), FnvV('S', h));
        } else {
            h = FnvV('P', h);
        }
    }
    return h;
}
}  // namespace

bool Capture(Data* out) {
    *out = Data{};
    out->level = VarString("WXD.Level.Current");
    out->landFile = VarString("Land.File");
    out->landTheme = VarString("Land.Theme");
    out->dataBank = VarString("GameLogic.DataBank");
    out->timeOfDay = VarString("Databank.TimeOfDay");
    out->levelDetails = VarString("LevelDetailsName");
    out->lastScheme = VarString("FE.LastSchemeUserSelected");

    uint8_t scheme[0x174];
    if (const uintptr_t p = VarObject("GM.SchemeData"); p && gamestate::Peek(p, scheme, sizeof scheme)) {
        uint32_t name;
        memcpy(&name, scheme + 0x14, 4);
        CString(name, &out->schemeName);
        uint64_t h = HashFields(scheme, 0x14, 0x20, {}, kFnvBasis);
        // 0x108 is the AssistedShotSettings reference (a heap pointer); the 26 integer settings follow it.
        out->scheme = Fnv(scheme + 0x10c, sizeof scheme - 0x10c, h);
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0x10c; i < sizeof scheme; ++i) {
            out->schemeRaw.push_back(kHex[scheme[i] >> 4]);
            out->schemeRaw.push_back(kHex[scheme[i] & 15]);
        }
        out->haveScheme = true;
    }

    uint8_t init[0x190];
    if (const uintptr_t p = VarObject("GM.GameInitData"); p && gamestate::Peek(p, init, sizeof init)) {
        out->init = Fnv(init + 0x18c, 4, HashFields(init, 0x14, 0x18c, {0x84, 0xcc, 0xec, 0x144}, kFnvBasis));
        out->haveInit = true;
        uint32_t teams;
        memcpy(&teams, init + 0x14, 4);
        constexpr uint32_t kName[4] = {0x2c, 0x94, 0xf4, 0x14c}, kWorms[4] = {0x34, 0x9c, 0xfc, 0x154};
        for (uint32_t i = 0; i < teams && i < 4; ++i) {
            Team t;
            uint32_t name;
            memcpy(&name, init + kName[i], 4);
            CString(name, &t.name);
            memcpy(&t.worms, init + kWorms[i], 4);
            out->teams.push_back(std::move(t));
        }
    }
    return !out->landFile.empty() || out->haveInit;
}
}  // namespace melange::wormsign::setup
