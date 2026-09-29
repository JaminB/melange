#include "wormsign/fpu.h"

#include <xmmintrin.h>

#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/dllcall.h"
#include "core/log.h"
#include "melange/jlog.h"

namespace melange::wormsign::fpu {
namespace {
constexpr size_t kKeep = 64;
std::mutex g_mu;
Event g_events[kKeep];
size_t g_count = 0;
uint32_t g_changes = 0;
bool g_have = false;
uint16_t g_cw = kExpectedCw;
uint32_t g_mx = 0;

std::string Hex(uint32_t v, int w) {
    char b[16];
    snprintf(b, sizeof b, "\"%0*x\"", w, v);
    return b;
}
}  // namespace

void Tick(uint32_t serial, uint32_t tick, uint16_t cw) { Observe(serial, tick, cw, _mm_getcsr()); }

void Observe(uint32_t serial, uint32_t tick, uint16_t cw, uint32_t mxcsr) {
    if (g_have && cw == g_cw && ((mxcsr ^ g_mx) & ~kMxcsrFlags) == 0) return;
    const bool first = !g_have;
    const uint16_t prevCw = g_cw;
    const uint32_t prevMx = first ? mxcsr : g_mx;
    g_have = true;
    g_cw = cw;
    g_mx = mxcsr;
    if (first && cw == kExpectedCw) return;
    const char* last = dllcall::Last();
    {
        std::lock_guard lk(g_mu);
        ++g_changes;
        if (g_count < kKeep) {
            Event& e = g_events[g_count++];
            e = {serial, tick, cw, prevCw, mxcsr, prevMx, {}};
            snprintf(e.lastCall, sizeof e.lastCall, "%s", last);
        }
    }
    if (cw != prevCw || first) {
        if (cw != kExpectedCw) {
            LOG_WARN("[wormsign] FPU control word %04x at tick %u of match %u (expected %04x, was %04x); last hooked "
                     "DLL call: %s",
                     cw, tick, serial, kExpectedCw, prevCw, last);
            jlog::Rec("wormsign", jlog::Level::Warn, "fpu control word changed")
                .Uint("serial", serial).Uint("tick", tick).Hex("cw", cw).Hex("prev", prevCw).Hex("mxcsr", mxcsr)
                .Str("lastCall", last);
        } else {
            LOG_INFO("[wormsign] FPU control word back to %04x at tick %u of match %u", cw, tick, serial);
            jlog::Rec("wormsign", jlog::Level::Info, "fpu control word restored")
                .Uint("serial", serial).Uint("tick", tick).Hex("cw", cw);
        }
    } else {
        LOG_INFO("[wormsign] MXCSR %08x at tick %u of match %u (was %08x); last hooked DLL call: %s", mxcsr, tick, serial,
                 prevMx, last);
        jlog::Rec("wormsign", jlog::Level::Info, "mxcsr changed")
            .Uint("serial", serial).Uint("tick", tick).Hex("mxcsr", mxcsr).Hex("prev", prevMx).Str("lastCall", last);
    }
}

size_t Events(Event* out, size_t max) {
    std::lock_guard lk(g_mu);
    const size_t n = g_count < max ? g_count : max;
    memcpy(out, g_events, n * sizeof(Event));
    return n;
}

uint16_t Cw() { return g_cw; }
uint32_t Mxcsr() { return g_mx; }
uint32_t Changes() {
    std::lock_guard lk(g_mu);
    return g_changes;
}

std::string NoteJson() {
    Event ev[kKeep];
    const size_t n = Events(ev, kKeep);
    std::string s = "{\"fpu\":{\"expected\":" + Hex(kExpectedCw, 4) + ",\"cw\":" + Hex(g_cw, 4) +
                    ",\"mxcsr\":" + Hex(g_mx, 8) + ",\"changes\":" + std::to_string(Changes()) + ",\"events\":[";
    for (size_t i = 0; i < n; ++i) {
        const Event& e = ev[i];
        if (i) s += ",";
        s += "{\"serial\":" + std::to_string(e.serial) + ",\"tick\":" + std::to_string(e.tick) + ",\"cw\":" +
             Hex(e.cw, 4) + ",\"prevCw\":" + Hex(e.prevCw, 4) + ",\"mxcsr\":" + Hex(e.mxcsr, 8) + ",\"lastCall\":\"";
        for (const char* p = e.lastCall; *p; ++p)
            if (*p != '"' && *p != '\\' && static_cast<unsigned char>(*p) >= 0x20) s += *p;
        s += "\"}";
    }
    s += "]}}";
    return s;
}

void ResetForTest() {
    std::lock_guard lk(g_mu);
    g_count = 0;
    g_changes = 0;
    g_have = false;
    g_cw = kExpectedCw;
    g_mx = 0;
}
}  // namespace melange::wormsign::fpu
