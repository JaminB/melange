// EngineLog: mirrors every line the engine writes to XOM<n>-<PC>.log into WUMFix.log, so engine
// messages (including the netcode's "Session no longer viable..." lines) interleave with our traces.
#include <safetyhook.hpp>

#include <cstring>
#include <string>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"

namespace {
// void __cdecl XDebugOutSink(const char* line) - the final sink behind XDebugOutStream::Write (see re-notes §5).
constexpr uintptr_t kSink = 0x645c39;
SafetyHookInline g_hook;
std::string g_filter;  // empty = everything

// The engine streams a line in fragments ("  14 | ", "+ ", text, "\n"); reassemble per thread.
void Emit(std::string& line) {
    // Drop the engine's "    14 | + " prefix (frame counter, separator, category marker).
    size_t a = line.find_first_not_of(' ');
    size_t d = line.find_first_not_of("0123456789", a == std::string::npos ? 0 : a);
    if (a != std::string::npos && d != a && d != std::string::npos && line.compare(d, 3, " | ") == 0) {
        a = d + 3;
        if (line.compare(a, 2, "+ ") == 0) a += 2;
    }
    if (a != std::string::npos && a < line.size() && (g_filter.empty() || line.find(g_filter) != std::string::npos))
        wf::log::Write("ENG  ", "%s", line.c_str() + a);
    line.clear();
}

void __cdecl HookSink(const char* text) {
    g_hook.ccall<void>(text);
    if (!text) return;
    thread_local std::string line;
    for (const char* p = text; *p && p < text + 256; ++p) {
        if (*p == '\n') {
            Emit(line);
        } else if (*p != '\r') {
            line += *p;
        }
    }
    if (line.size() > 4096) Emit(line);
}

class EngineLog final : public wf::Module {
public:
    const char* Name() const override { return "EngineLog"; }
    const char* Description() const override { return "mirrors the engine's XOM log into WUMFix.log"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 12; }

    bool Install() override {
        g_filter = wf::config::GetString(Name(), "Filter", "");
        wf::config::EnsureKey(Name(), "Filter", "");
        if (!wf::mem::Expect(kSink, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x1c})) return false;
        g_hook = safetyhook::create_inline(kSink, &HookSink);
        return static_cast<bool>(g_hook);
    }
    void Uninstall() override { g_hook = {}; }
};
}  // namespace

WUMFIX_MODULE(EngineLog);
