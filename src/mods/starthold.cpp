#include "mods/starthold.h"

#include <safetyhook.hpp>

#include <atomic>
#include <vector>

#include "core/events.h"
#include "core/log.h"
#include "weapons/engine.h"

namespace melange::mods::starthold {
namespace {
struct Reason {
    int handle;
    std::string name;
    ReasonFn fn;
    void* user;
    bool holding;
    uint32_t frames;
};
std::vector<Reason> g_reasons;
int g_next = 1;
bool g_subscribed = false;

SafetyHookMid g_hook;
bool g_hookFailed = false;
std::atomic<bool> g_hold{false};
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_pendingFrames{0};

// While holding, the players-with-a-team vs players compare is made to fail.
void OnStartCheck(safetyhook::Context& c) {
    if (!g_hold.load(std::memory_order_relaxed) || c.edi != c.eax || c.edi <= 1) return;
    c.eax = c.edi + 1;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_pendingFrames.fetch_add(1, std::memory_order_relaxed);
}

void Evaluate() {
    bool any = false;
    const uint32_t pending = g_pendingFrames.exchange(0, std::memory_order_relaxed);
    for (auto& r : g_reasons) {
        if (r.holding) r.frames += pending;
        std::string why;
        const bool h = r.fn && r.fn(&why, r.user);
        if (h != r.holding) LOG_INFO("[starthold] %s %s%s%s", r.name.c_str(), h ? "holds the start" : "released",
                                     h && !why.empty() ? ": " : "", h ? why.c_str() : "");
        r.holding = h;
        any |= h;
    }
    if (any && !g_hook && !g_hookFailed) {
        if (!weapons::engine::Mid(g_hook, weapons::engine::kStartCheck, &OnStartCheck, "start refusal")) {
            g_hookFailed = true;
            LOG_ERROR("[starthold] the start hold is unavailable: the hook could not be created");
        }
    }
    g_hold = any && g_hook;
    if (g_hook) weapons::engine::Enable(g_hook, any);
}
}  // namespace

int Add(const char* name, ReasonFn fn, void* user) {
    if (!fn) return 0;
    if (!g_subscribed) {
        g_subscribed = true;
        events::Subscribe(events::Event::Frame, [] {
            if (!g_reasons.empty() || g_hold.load()) Evaluate();
        });
    }
    const int h = g_next++;
    g_reasons.push_back({h, name ? name : "?", fn, user, false, 0});
    return h;
}

void Remove(int handle) {
    std::erase_if(g_reasons, [handle](const Reason& r) { return r.handle == handle; });
}

bool Holding(std::string* why) {
    bool any = false;
    for (auto& r : g_reasons) {
        std::string w;
        if (!r.fn || !r.fn(&w, r.user)) continue;
        if (why) *why += (why->empty() ? "" : "; ") + r.name + (w.empty() ? "" : ": " + w);
        any = true;
    }
    return any;
}

bool Available() { return !g_hookFailed; }

bool HookEnabled(int* state) {
    const int s = g_hook ? static_cast<int>(g_hook.enabled()) : -1;
    if (state) *state = s;
    return s == 1;
}

uint32_t HeldFrames() { return g_frames.load(std::memory_order_relaxed); }

uint32_t HeldFrames(const char* name) {
    for (auto& r : g_reasons)
        if (name && r.name == name) return r.frames + (r.holding ? g_pendingFrames.load(std::memory_order_relaxed) : 0);
    return 0;
}
}  // namespace melange::mods::starthold
