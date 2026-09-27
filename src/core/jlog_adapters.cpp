// Component C's adapters: the WF_ log tap, the raw event-bus -> "event" mirror, the semantic game-event
// mapper, the net lifecycle mirror, and the Logging module that wires all of it up plus the viewer panels.
// Public contract: src/sdk/wumfix/jlog.h. See docs/m0-design.md SS2.5, SS3 "C".
//
// Written against the frozen wumfix/bus.h (B) and wumfix/overlay.h (A) contracts; this file implements
// neither B nor A. In this worktree bus.cpp/overlay.cpp are still no-op stand-ins (see the notes at the top of
// those two files), so SubscribeAll/SubscribeName/AddPanel below never actually fire at runtime here - they
// will once this branch merges after B and A, per docs/m0-design.md SS4.
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
#include "core/module.h"
#include "tools/log_viewer.h"
#include "version.h"
#include "wumfix/bus.h"
#include "wumfix/jlog.h"
#include "wumfix/testcmd.h"

namespace {
using wf::jlog::Level;
using wf::jlog::Rec;

// -------------------------------------------------------------------------------------- 1. WF_ tap (adapter 1)
// wf::log::Write() calls this with the already-formatted message, before the timestamp/thread-id prefix is
// added. jlog must never call back into wf::log (that would recurse).
void OnLogTap(const char* levelStr, const char* msgC) {
    std::string_view lv(levelStr ? levelStr : "");
    std::string_view text(msgC ? msgC : "");

    // EngineLog's XomOnline mirror embeds the real engine level in the text itself (see engine_log.cpp);
    // pull it back out so it lands in its own "xomonline" category at the right level, per the SS3 "C"
    // category table, instead of generically as "engine"/info.
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

    // WF_* lines that carry the "[tag] ..." convention used throughout the codebase (net_session.cpp,
    // automation.cpp, ...) become "module" records with data.tag, per the SS3 "C" category table.
    std::string_view tag;
    if (!text.empty() && text.front() == '[') {
        size_t close = text.find(']');
        if (close != std::string_view::npos) {
            tag = text.substr(1, close - 1);
            cat = "module";
        }
    }
    if (!wf::jlog::Enabled(cat, lvl)) return;
    Rec rec(cat, lvl, text);
    if (!tag.empty()) rec.Str("tag", tag);
    rec.Emit();
}

// -------------------------------------------------------------------------------------- 2. bus -> event
class RecJsonOut final : public wf::bus::JsonOut {
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

// Shared by both the raw event mirror and the game mapper: writes the common MessageView fields, then either
// a decoded payload or (unknown vtable) the size plus first 32 bytes as hex - the fallback the spec assigns
// to component C.
void LogRawEvent(const wf::bus::MessageView& m) {
    if (!wf::jlog::busfilter::ShouldLog(m.name)) return;
    if (!wf::jlog::Enabled("event", Level::Info)) return;
    Rec rec("event", Level::Info, "event");
    rec.Uint("id", m.id);
    rec.Str("name", m.name);
    rec.Str("class", m.className);
    rec.Str("path", m.path == wf::bus::Path::Post ? "post" : "deliver");
    rec.Int("handle", m.handle);
    rec.Hex("caller", m.caller);

    RecJsonOut out(rec);
    if (!wf::bus::Decode(m, out)) {
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

void OnBusPost(const wf::bus::MessageView& m, void*) { LogRawEvent(m); }
void OnBusDeliver(const wf::bus::MessageView& m, void*) {
    if (m.fromPost) return;  // already logged once via the matching Post (docs/m0-design.md SS3 "C", adapter 2)
    LogRawEvent(m);
}

// -------------------------------------------------------------------------------------- 3. game mapper
std::atomic<int> g_turnNumber{0};
std::atomic<uint64_t> g_lastTurnEndTick{0};

// Weapon.Fired is always posted from the same call site inside the shared helper (see docs/m0-design.md
// SS1.3), so m.caller never distinguishes which weapon fired. The caller's caller - the actual weapon-class
// function - is one frame further up our own stack when this handler runs. [I] like the spec's own note, this
// may be off by one frame; it is not exercised in this offline build (bus.cpp is a stub) and should be
// checked against the real game before relying on it (see the M0 report for component C).
constexpr uintptr_t kWeaponFiredPostSite = 0x549cc2;

std::string CaptureShotVia() {
    void* frames[4] = {};
    USHORT n = CaptureStackBackTrace(2, 4, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        auto addr = reinterpret_cast<uintptr_t>(frames[i]);
        if (addr) return wf::game::DescribeAddress(addr);
    }
    return "unknown";
}

void ResetTurnCounter() {
    g_turnNumber = 0;
    g_lastTurnEndTick = 0;
}

void OnTurnStarted(const wf::bus::MessageView&, void*) {
    if (GetTickCount64() - g_lastTurnEndTick.load() > 30000) g_turnNumber = 0;
    int turn = ++g_turnNumber;
    Rec("game", Level::Info, "turn_start").Int("turn", turn).Emit();
}
void OnTurnEnded(const wf::bus::MessageView&, void*) {
    g_lastTurnEndTick.store(GetTickCount64());
    Rec("game", Level::Info, "turn_end").Int("turn", g_turnNumber.load()).Emit();
}
void OnWeaponFired(const wf::bus::MessageView&, void*) {
    Rec("game", Level::Info, "shot").Hex("caller", kWeaponFiredPostSite).Str("via", CaptureShotVia()).Emit();
}
void OnDeathQueue(const wf::bus::MessageView& m, void*) {
    uint32_t taskId = 0;
    m.Get<uint32_t>(8, taskId);  // TaskIDMessage payload (docs/m0-design.md SS1.2/SS1.3)
    Rec("game", Level::Info, "death").Uint("taskId", taskId).Emit();
}
void OnWormDied(const wf::bus::MessageView& m, void*) {
    // Worm.Died's payload is not decoded by the spec; the Deliver target handle identifies the worm entity
    // instead. [I] approximation, noted in the M0 report for component C.
    Rec("game", Level::Info, "worm_cleanup").Int("taskId", m.handle).Bool("teardown", true).Emit();
}
void OnWormDamaged(const wf::bus::MessageView&, void*) { Rec("game", Level::Info, "damage").Emit(); }
void OnExplosion(const wf::bus::MessageView& m, void*) {
    Rec rec("game", Level::Info, "explosion");
    RecJsonOut out(rec);
    wf::bus::Decode(m, out);
    rec.Emit();
}

// -------------------------------------------------------------------------------------- 4. net (events tap)
void InstallNetTap() {
    using wf::events::Event;
    wf::events::Subscribe(Event::MatchStart, [] {
        ResetTurnCounter();
        Rec("net", Level::Info, "match_start").Emit();
    });
    wf::events::Subscribe(Event::MatchEnd, [] { Rec("net", Level::Info, "match_end").Emit(); });
    wf::events::Subscribe(Event::LobbyEnter, [] { Rec("net", Level::Info, "lobby_enter").Emit(); });
    wf::events::Subscribe(Event::LobbyLeave, [] { Rec("net", Level::Info, "lobby_leave").Emit(); });
}

// -------------------------------------------------------------------------------------- test verbs
bool g_allowCrashTest = false;

bool CmdMark(std::string_view args, void*) {
    Rec("test", Level::Info, "mark").Str("text", args).Emit();
    return true;
}
bool CmdFlush(std::string_view, void*) { return wf::jlog::Flush(2000); }
bool CmdCrashTest(std::string_view, void*) {
    if (!g_allowCrashTest) {
        WF_WARN("[jlog] jlog.crashtest blocked: [Logging] AllowCrashTest=0");
        return false;
    }
    WF_WARN("[jlog] jlog.crashtest: forcing an access violation on the main thread");
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

// -------------------------------------------------------------------------------------- Logging module
class Logging final : public wf::Module {
public:
    const char* Name() const override { return "Logging"; }
    const char* Description() const override { return "structured JSONL event log, adapters and overlay viewer"; }
    int Order() const override { return 1; }  // right after Diagnostics (0)

    bool Install() override {
        wf::jlog::internal::Options opt;
        std::string dir = wf::config::GetString(Name(), "Dir", "");
        wf::config::EnsureKey(Name(), "Dir", "");
        opt.rootOverride = Widen(dir);
        opt.fallbackRoot = wf::game::GameDir() + L"\\WUMFix\\logs";

        opt.levelsSpec = wf::config::GetString(Name(), "Levels", "*:info,event:info,engine:info");
        wf::config::EnsureKey(Name(), "Levels", "*:info,event:info,engine:info");
        opt.maxFileMB = static_cast<uint32_t>(Int("MaxFileMB", 32));
        opt.maxSessions = static_cast<uint32_t>(Int("MaxSessions", 20));
        opt.maxTotalMB = static_cast<uint32_t>(Int("MaxTotalMB", 512));
        opt.tailCapacity = static_cast<size_t>(Int("TailLines", 5000));
        g_allowCrashTest = Bool("AllowCrashTest", false);

        std::string deny = wf::config::GetString(Name(), "EventDeny",
            "Camera.HasUpdated,Land.CheckVoxel,HeldAccessory.Hide,Acting.Trigger,AI.IssueWormCommand,"
            "Input.SomeInputFrom,Particle.DelGraphicalEmitter,sys:0x1004");
        wf::config::EnsureKey(Name(), "EventDeny",
            "Camera.HasUpdated,Land.CheckVoxel,HeldAccessory.Hide,Acting.Trigger,AI.IssueWormCommand,"
            "Input.SomeInputFrom,Particle.DelGraphicalEmitter,sys:0x1004");
        std::string allow = wf::config::GetString(Name(), "EventAllow", "");
        wf::config::EnsureKey(Name(), "EventAllow", "");
        wf::jlog::busfilter::Init(deny, allow);

        if (!wf::jlog::internal::Init(opt)) {
            WF_ERROR("[jlog] could not open a writable log directory (tried '%s' and fallback '%s')",
                     wf::game::Narrow(opt.rootOverride).c_str(), wf::game::Narrow(opt.fallbackRoot).c_str());
            return false;
        }

        // The very first record of the session file (docs/m0-design.md SS3 "C", record schema v1).
        Rec("session", Level::Info, "start")
            .Str("version", WUMFIX_VERSION)
            .Str("exeSha256", wf::game::Exe().sha256)
            .Uint("pid", GetCurrentProcessId())
            .Emit();

        wf::log::SetTap(&OnLogTap);  // adapter 1

        wf::bus::SubscribeAll(wf::bus::Path::Post, &OnBusPost);  // adapter 2
        wf::bus::SubscribeAll(wf::bus::Path::Deliver, &OnBusDeliver);

        wf::bus::SubscribeName("GameLogic.Turn.Started", wf::bus::Path::Post, &OnTurnStarted);  // adapter 3
        wf::bus::SubscribeName("GameLogic.Turn.Ended", wf::bus::Path::Post, &OnTurnEnded);
        wf::bus::SubscribeName("Weapon.Fired", wf::bus::Path::Post, &OnWeaponFired);
        wf::bus::SubscribeName("GameLogic.AddMeToDeathQueue", wf::bus::Path::Post, &OnDeathQueue);
        wf::bus::SubscribeName("Worm.Died", wf::bus::Path::Post, &OnWormDied);
        wf::bus::SubscribeName("Worm.Damaged", wf::bus::Path::Post, &OnWormDamaged);
        wf::bus::SubscribeName("Explosion", wf::bus::Path::Post, &OnExplosion);

        InstallNetTap();  // adapter 4 (net_session.cpp itself carries the "state" record, see its own edit)

        wf::logviewer::Install();  // adapter 5

        wf::testcmd::Register("jlog.mark", &CmdMark);
        wf::testcmd::Register("jlog.flush", &CmdFlush);
        wf::testcmd::Register("jlog.crashtest", &CmdCrashTest);

        WF_INFO("logging: session %s, root %s", wf::jlog::CurrentSession().id.c_str(),
                wf::game::Narrow(wf::jlog::CurrentSession().root).c_str());
        return true;
    }
};

}  // namespace

WUMFIX_MODULE(Logging);
