// Logging module: feeds the structured log from the log tap, the event bus (raw and as game events), the net
// lifecycle events and frame gaps, and installs the viewer panels. Public API: melange/jlog.h.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/jlog_bus_filter.h"
#include "core/jlog_internal.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "tools/log_viewer.h"
#include "version.h"
#include "melange/bus.h"
#include "melange/jlog.h"
#include "melange/testcmd.h"

namespace {
using melange::jlog::Level;
using melange::jlog::Rec;

// Log tap. Must never call back into melange::log (recursion).
void OnLogTap(const char* levelStr, const char* msgC) {
    std::string_view lv(levelStr ? levelStr : "");
    std::string_view text(msgC ? msgC : "");

    // EngineLog's XomOnline lines carry the engine level in the text; recover it for the "xomonline" category.
    if (text.rfind("XomOnline[", 0) == 0) {
        size_t close = text.find(']');
        Level lvl = Level::Info;
        if (close != std::string_view::npos) {
            std::string_view word = text.substr(10, close - 10);
            if (word == "WARNING") lvl = Level::Warn;
            else if (word == "FAILURE") lvl = Level::Error;
        }
        Rec(std::string_view("xomonline"), lvl, text).Emit();
        return;
    }

    Level lvl = Level::Info;
    const char* cat = "core";
    if (lv == "WARN ") lvl = Level::Warn;
    else if (lv == "ERROR") lvl = Level::Error;
    else if (lv == "TRACE") lvl = Level::Trace;
    else if (lv == "ENG  ") { lvl = Level::Info; cat = "engine"; }

    // "[tag] ..." lines become "module" records with data.tag.
    std::string_view tag;
    if (!text.empty() && text.front() == '[') {
        size_t close = text.find(']');
        if (close != std::string_view::npos) {
            tag = text.substr(1, close - 1);
            cat = "module";
        }
    }
    if (!melange::jlog::Enabled(cat, lvl)) return;
    Rec rec(cat, lvl, text);
    if (!tag.empty()) rec.Str("tag", tag);
    rec.Emit();
}

// ---- Bus -> event
class RecJsonOut final : public melange::bus::JsonOut {
public:
    explicit RecJsonOut(Rec& rec) : rec_(rec) {}
    void Int(const char* k, int64_t v) override { rec_.Int(k, v); }
    void Uint(const char* k, uint64_t v) override { rec_.Uint(k, v); }
    void Hex(const char* k, uint64_t v) override { rec_.Hex(k, v); }
    void Float(const char* k, double v) override { rec_.Float(k, v); }
    void Str(const char* k, std::string_view v) override { rec_.Str(k, v); }
    void Vec3(const char* k, const float v[3]) override { rec_.Vec3(k, v); }

private:
    Rec& rec_;
};

// Common MessageView fields, then the decoded payload or, for an unknown class, the first 32 bytes as hex.
void LogRawEvent(const melange::bus::MessageView& m) {
    if (!melange::jlog::busfilter::ShouldLog(m.name)) return;
    if (!melange::jlog::Enabled("event", Level::Info)) return;
    Rec rec("event", Level::Info, "event");
    rec.Uint("id", m.id);
    rec.Str("name", m.name);
    rec.Str("class", m.className);
    rec.Str("path", m.path == melange::bus::Path::Post ? "post" : "deliver");
    rec.Int("handle", m.handle);
    rec.Hex("caller", m.caller);

    RecJsonOut out(rec);
    if (!melange::bus::Decode(m, out)) {
        rec.Uint("size", m.size);
        uint8_t buf[32] = {};
        uint32_t n = m.size < sizeof(buf) ? m.size : static_cast<uint32_t>(sizeof(buf));
        if (n && m.Read(0, buf, n)) {
            char hex[sizeof(buf) * 2 + 1];
            for (uint32_t i = 0; i < n; ++i) snprintf(hex + i * 2, 3, "%02x", buf[i]);
            hex[n * 2] = 0;
            rec.Str("bytes", hex);
        }
    }
    rec.Emit();
}

void OnBusPost(const melange::bus::MessageView& m, void*) { LogRawEvent(m); }
void OnBusDeliver(const melange::bus::MessageView& m, void*) {
    if (m.fromPost) return;  // already logged via the matching Post
    LogRawEvent(m);
}

// ---- Game events
std::atomic<int> g_turnNumber{0};
std::atomic<uint64_t> g_lastTurnEndTick{0};

// Weapon.Fired is always posted from the shared helper 0x549bb0, so the weapon is identified by the helper's caller.
// Our frames may omit frame pointers, so scan the raw stack: past the Post return address 0x549cc2, the first
// return address of a direct `call 0x549bb0`. Indirect calls are missed ("via" stays 0).
constexpr uintptr_t kWeaponFiredPostSite = 0x549cc2;
constexpr uintptr_t kWeaponFiredHelper = 0x549bb0;

uintptr_t CaptureShotVia() {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    const auto base = reinterpret_cast<uintptr_t>(tib->StackBase);
    uintptr_t here = 0;
    uintptr_t p = reinterpret_cast<uintptr_t>(&here) & ~uintptr_t(3);
    const uintptr_t end = (base - p > 0x8000) ? p + 0x8000 : base;
    bool seenPostSite = false;
    for (; p + 4 <= end; p += 4) {
        uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
        if (!seenPostSite) {
            seenPostSite = (v == kWeaponFiredPostSite);
            continue;
        }
        if (v < 0x401005 || v >= 0x800000) continue;  // WormsMayhem.exe code range
        uint8_t op = 0;
        int32_t rel = 0;
        if (!melange::mem::SafeRead(v - 5, &op, 1) || op != 0xE8) continue;
        if (!melange::mem::SafeRead(v - 4, &rel, 4)) continue;
        if (v + static_cast<uintptr_t>(rel) == kWeaponFiredHelper) return v;
    }
    return 0;
}

void ResetTurnCounter() {
    g_turnNumber = 0;
    g_lastTurnEndTick = 0;
}

void OnTurnStarted(const melange::bus::MessageView&, void*) {
    if (GetTickCount64() - g_lastTurnEndTick.load() > 30000) g_turnNumber = 0;
    int turn = ++g_turnNumber;
    Rec("game", Level::Info, "turn_start").Int("turn", turn).Emit();
}
void OnTurnEnded(const melange::bus::MessageView&, void*) {
    g_lastTurnEndTick.store(GetTickCount64());
    Rec("game", Level::Info, "turn_end").Int("turn", g_turnNumber.load()).Emit();
}
void OnWeaponFired(const melange::bus::MessageView&, void*) {
    Rec("game", Level::Info, "shot").Hex("caller", kWeaponFiredPostSite).Hex("via", CaptureShotVia()).Emit();
}
void OnDeathQueue(const melange::bus::MessageView& m, void*) {
    uint32_t taskId = 0;
    m.Get<uint32_t>(8, taskId);  // TaskIDMessage payload
    Rec("game", Level::Info, "death").Uint("taskId", taskId).Emit();
}
void OnWormDied(const melange::bus::MessageView& m, void*) {
    // Worm.Died is a TaskIDMessage (+8 = task id); on the Post path m.handle is always the post target.
    uint32_t taskId = 0;
    m.Get<uint32_t>(8, taskId);
    Rec("game", Level::Info, "worm_cleanup").Uint("taskId", taskId).Bool("teardown", true).Emit();
}
void OnWormDamaged(const melange::bus::MessageView&, void*) { Rec("game", Level::Info, "damage").Emit(); }
void OnExplosion(const melange::bus::MessageView& m, void*) {
    Rec rec("game", Level::Info, "explosion");
    RecJsonOut out(rec);
    melange::bus::Decode(m, out);
    rec.Emit();
}

// ---- Net lifecycle
void InstallNetTap() {
    using melange::events::Event;
    melange::events::Subscribe(Event::MatchStart, [] {
        ResetTurnCounter();
        Rec("net", Level::Info, "match_start").Emit();
    });
    melange::events::Subscribe(Event::MatchEnd, [] { Rec("net", Level::Info, "match_end").Emit(); });
    melange::events::Subscribe(Event::LobbyEnter, [] { Rec("net", Level::Info, "lobby_enter").Emit(); });
    melange::events::Subscribe(Event::LobbyLeave, [] { Rec("net", Level::Info, "lobby_leave").Emit(); });
}

// ---- Frame gaps: every frame interval above 100 ms becomes a "core"/"frame_gap" record.
LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_lastFrameQpc{};

void OnFrameForGaps() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_lastFrameQpc.QuadPart && g_qpcFreq.QuadPart) {
        double ms = (now.QuadPart - g_lastFrameQpc.QuadPart) * 1000.0 / static_cast<double>(g_qpcFreq.QuadPart);
        if (ms > 100.0 && melange::jlog::Enabled("core", Level::Info))
            Rec("core", Level::Info, "frame_gap").Float("ms", ms).Uint("frame", melange::events::FrameCount()).Emit();
    }
    g_lastFrameQpc = now;
}

// ---- Test verbs
bool g_allowCrashTest = false;

bool CmdMark(std::string_view args, void*) {
    Rec("test", Level::Info, "mark").Str("text", args).Emit();
    return true;
}
bool CmdFlush(std::string_view, void*) { return melange::jlog::Flush(2000); }
bool CmdStats(std::string_view, void*) {
    auto st = melange::jlog::GetStats();
    uint64_t samples = 0;
    double p95 = melange::jlog::internal::EmitP95Us(&samples);
    LOG_INFO("[jlog] stats: records=%llu dropped=%llu bytes=%llu filesRotated=%llu mainEmitP95Us=%.2f (n=%llu)",
            static_cast<unsigned long long>(st.records), static_cast<unsigned long long>(st.dropped),
            static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.filesRotated), p95,
            static_cast<unsigned long long>(samples));
    return true;
}
bool CmdCrashTest(std::string_view, void*) {
    if (!g_allowCrashTest) {
        LOG_WARN("[jlog] jlog.crashtest blocked: [Logging] AllowCrashTest=0");
        return false;
    }
    LOG_WARN("[jlog] jlog.crashtest: forcing an access violation on the main thread");
    *reinterpret_cast<volatile int*>(0) = 1;
    return true;  // unreachable
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ---- Logging module
class Logging final : public melange::Module {
public:
    const char* Name() const override { return "Logging"; }
    const char* Description() const override { return "structured JSONL event log, adapters and overlay viewer"; }
    int Order() const override { return 1; }  // right after Diagnostics (0)

    bool Install() override {
        melange::jlog::internal::Options opt;
        std::string dir = melange::config::GetString(Name(), "Dir", "");
        melange::config::EnsureKey(Name(), "Dir", "");
        opt.rootOverride = Widen(dir);
        opt.fallbackRoot = melange::game::GameDir() + L"\\Melange\\logs";

        opt.levelsSpec = melange::config::GetString(Name(), "Levels", "*:info,event:info,engine:info");
        melange::config::EnsureKey(Name(), "Levels", "*:info,event:info,engine:info");
        opt.maxFileMB = static_cast<uint32_t>(Int("MaxFileMB", 32));
        opt.maxSessions = static_cast<uint32_t>(Int("MaxSessions", 20));
        opt.maxTotalMB = static_cast<uint32_t>(Int("MaxTotalMB", 512));
        opt.tailCapacity = static_cast<size_t>(Int("TailLines", 5000));
        g_allowCrashTest = Bool("AllowCrashTest", false);

        std::string deny = melange::config::GetString(Name(), "EventDeny",
            "Camera.HasUpdated,Land.CheckVoxel,HeldAccessory.Hide,Acting.Trigger,AI.IssueWormCommand,"
            "Input.SomeInputFrom,Particle.DelGraphicalEmitter,sys:0x1004");
        melange::config::EnsureKey(Name(), "EventDeny",
            "Camera.HasUpdated,Land.CheckVoxel,HeldAccessory.Hide,Acting.Trigger,AI.IssueWormCommand,"
            "Input.SomeInputFrom,Particle.DelGraphicalEmitter,sys:0x1004");
        std::string allow = melange::config::GetString(Name(), "EventAllow", "");
        melange::config::EnsureKey(Name(), "EventAllow", "");
        melange::jlog::busfilter::Init(deny, allow);

        if (!melange::jlog::internal::Init(opt)) {
            LOG_ERROR("[jlog] could not open a writable log directory (tried '%s' and fallback '%s')",
                     melange::game::Narrow(opt.rootOverride).c_str(), melange::game::Narrow(opt.fallbackRoot).c_str());
            return false;
        }

        // First record of the session file.
        Rec("session", Level::Info, "start")
            .Str("version", MELANGE_VERSION)
            .Str("exeSha256", melange::game::Exe().sha256)
            .Uint("pid", GetCurrentProcessId())
            .Emit();

        melange::log::SetTap(&OnLogTap);

        melange::bus::SubscribeAll(melange::bus::Path::Post, &OnBusPost);
        melange::bus::SubscribeAll(melange::bus::Path::Deliver, &OnBusDeliver);

        melange::bus::SubscribeName("GameLogic.Turn.Started", melange::bus::Path::Post, &OnTurnStarted);
        melange::bus::SubscribeName("GameLogic.Turn.Ended", melange::bus::Path::Post, &OnTurnEnded);
        melange::bus::SubscribeName("Weapon.Fired", melange::bus::Path::Post, &OnWeaponFired);
        melange::bus::SubscribeName("GameLogic.AddMeToDeathQueue", melange::bus::Path::Post, &OnDeathQueue);
        melange::bus::SubscribeName("Worm.Died", melange::bus::Path::Post, &OnWormDied);
        melange::bus::SubscribeName("Worm.Damaged", melange::bus::Path::Post, &OnWormDamaged);
        melange::bus::SubscribeName("Explosion", melange::bus::Path::Post, &OnExplosion);

        InstallNetTap();

        QueryPerformanceFrequency(&g_qpcFreq);
        melange::events::Subscribe(melange::events::Event::Frame, &OnFrameForGaps);

        melange::logviewer::Install();

        melange::testcmd::Register("jlog.mark", &CmdMark);
        melange::testcmd::Register("jlog.flush", &CmdFlush);
        melange::testcmd::Register("jlog.stats", &CmdStats);
        melange::testcmd::Register("jlog.crashtest", &CmdCrashTest);

        LOG_INFO("logging: session %s, root %s", melange::jlog::CurrentSession().id.c_str(),
                melange::game::Narrow(melange::jlog::CurrentSession().root).c_str());
        return true;
    }
};

}  // namespace

MELANGE_MODULE(Logging);
