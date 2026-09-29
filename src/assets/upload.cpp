#include "assets/upload.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/mem.h"
#include "weapons/engine.h"

namespace melange::assets::upload {
namespace {
namespace engine = weapons::engine;

struct Patcher {
    int handle;
    std::string name;
    PatchFn fn;
    void* user;
};
std::vector<Patcher> g_patchers;
int g_next = 1;
SafetyHookMid g_hook;
Stats g_stats{};

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

std::string Stem(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s.erase(0, slash + 1);
    for (const char* ext : {".tga", ".png", ".dds", ".bmp"})
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ext) == 0) s.resize(s.size() - 4);
    return s;
}

void OnUpload(safetyhook::Context& c) {
    const uintptr_t img = Rd<uintptr_t>(c.esp + 4);
    if (!img) return;
    ++g_stats.uploadsSeen;
    const std::string name = engine::ReadCString(Rd<uintptr_t>(img + 0x14), 96);
    if (name.empty()) return;
    const std::string stem = Stem(name);
    bool any = false;
    for (auto& p : g_patchers) any |= p.name == stem;
    if (!any) return;
    const uint16_t w = Rd<uint16_t>(img + 0x2c), h = Rd<uint16_t>(img + 0x2e);
    const uint32_t fmt = Rd<uint32_t>(img + 0x20);
    const uintptr_t d = Rd<uintptr_t>(img + 0x34);
    const uint32_t size = d ? Rd<uint32_t>(d + 0x18) : 0;
    if (!d || !size || size > (64u << 20)) return;
    uint8_t* px = reinterpret_cast<uint8_t*>(d + 0x20);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(px, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        reinterpret_cast<uintptr_t>(px) + size > reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    const auto list = g_patchers;
    for (auto& p : list) {
        if (p.name != stem) continue;
        try {
            p.fn(name.c_str(), w, h, fmt, px, size, p.user);
        } catch (...) {
            LOG_ERROR("[assets] an upload patcher for '%s' threw", name.c_str());
        }
    }
    ++g_stats.uploadsPatched;
    g_stats.msLastPatch = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Refresh() {
    g_stats.patchers = static_cast<uint32_t>(g_patchers.size());
    if (g_hook) engine::Enable(g_hook, !g_patchers.empty());
    g_stats.hooked = g_hook && g_hook.enabled();
}
}  // namespace

bool Available() { return engine::SiteIntact(engine::kUpload) || static_cast<bool>(g_hook); }

int AddPatcher(const char* imageName, PatchFn fn, void* user) {
    if (!imageName || !*imageName || !fn) return 0;
    if (!g_hook && !engine::Mid(g_hook, engine::kUpload, &OnUpload, "texture upload")) return 0;
    g_patchers.push_back({g_next, Stem(imageName), fn, user});
    Refresh();
    LOG_INFO("[assets] upload patcher %d on '%s'", g_next, imageName);
    return g_next++;
}

void RemovePatcher(int handle) {
    std::erase_if(g_patchers, [handle](const Patcher& p) { return p.handle == handle; });
    Refresh();
}

Stats GetStats() {
    g_stats.hooked = g_hook && g_hook.enabled();
    return g_stats;
}
}  // namespace melange::assets::upload
