// EngineLog: mirrors every line the engine writes to XOM<n>-<PC>.log into Melange.log, so engine
// messages (including the netcode's "Session no longer viable..." lines) interleave with our traces.
#include <safetyhook.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"

namespace {
// void __cdecl XDebugOutSink(const char* line) - the final sink behind XDebugOutStream::Write (see re-notes §5).
constexpr uintptr_t kSink = 0x645c39;
SafetyHookInline g_hook;
SafetyHookMid g_onlineHook;

// void __cdecl XomOnlineLog(obj, const char* tag, int level, const char* fmt, va_list) (0x41e3fd, re-notes §5).
// Retail only enables the stream carrying the level marker ("*** FAILURE ***"); the tag and message go to a
// disabled stream, so they are reconstructed here.
constexpr uintptr_t kOnlineLog = 0x41e3fd;

void OnOnlineLog(safetyhook::Context& c) {
    auto arg = [&](int i) { return *reinterpret_cast<uintptr_t*>(c.esp + 4 + 4 * i); };
    const char* tag = reinterpret_cast<const char*>(arg(1));
    int level = static_cast<int>(arg(2));
    const char* fmt = reinterpret_cast<const char*>(arg(3));
    // Per-packet chatter (several lines a second while connected).
    if (level < 2 && tag && (!strcmp(tag, "FlushSendStore") || !strcmp(tag, "ProcessHeartBeat"))) return;
    char msg[1024] = "";
    if (fmt) _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, reinterpret_cast<va_list>(arg(4)));
    static const char* kLevel[] = {"info", "info", "WARNING", "FAILURE"};
    melange::log::Write("ENG  ", "XomOnline[%s] %s: %s", kLevel[level & 3], tag ? tag : "", msg);
}
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
        melange::log::Write("ENG  ", "%s", line.c_str() + a);
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

class EngineLog final : public melange::Module {
public:
    const char* Name() const override { return "EngineLog"; }
    const char* Description() const override { return "mirrors the engine's XOM log into Melange.log"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 12; }

    bool Install() override {
        g_filter = melange::config::GetString(Name(), "Filter", "");
        melange::config::EnsureKey(Name(), "Filter", "");
        if (!melange::mem::Expect(kSink, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x1c})) return false;
        g_hook = safetyhook::create_inline(kSink, &HookSink);
        if (Bool("OnlineLog", true) && melange::mem::Expect(kOnlineLog, {0x55, 0x8b, 0xec, 0x81, 0xec, 0x30, 0x04}))
            g_onlineHook = safetyhook::create_mid(kOnlineLog, &OnOnlineLog);
        return static_cast<bool>(g_hook);
    }
    void Uninstall() override {
        g_onlineHook = {};
        g_hook = {};
    }
};
}  // namespace

MELANGE_MODULE(EngineLog);
