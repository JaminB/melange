// update.*: Melange.exe updating itself. One check at start (unless turned off in Settings, which also turns off the
// game's daily check) and on request from Settings, a background download
// into %LOCALAPPDATA%\Melange\updates\<version>\, and "Restart to update", which hands over to the new Melange.exe.
#include "launcher/update_host.h"

#include <windows.h>

#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "core/thread_guard.h"
#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/setup/detect.h"
#include "launcher/setup/running.h"
#include "launcher/store_host.h"
#include "launcher/util.h"
#include "launcher/window.h"
#include "oasis/standalone/register.h"
#include "tools/json_mini.h"
#include "version.h"

namespace melange::launcher {
void OnMelangeUpdated(const std::wstring& gameDir) {
    // Plugins that needed the old version, or can't load under this one, are set aside or updated now rather than
    // failing in the next game.
    LOG_INFO("[update] now Melange %s; game folder %ls", MELANGE_VERSION, gameDir.empty() ? L"(none)" : gameDir.c_str());
    storehost::RequestSweep(MELANGE_VERSION);
}

namespace updatehost {
namespace {
using oasis::Call;
using oasis::Result;

// idle: nothing known | checking | downloading | ready: staged and verified | current: up to date | error: a check
// the user asked for failed (an automatic one fails silently, to the log only)
struct State {
    std::string phase = "idle";
    std::string latest, htmlUrl, error, lastCheck;
    uint64_t got = 0, total = 0;
    bool autoCheck = true;   // Settings › Updates › Check for updates automatically
    bool haveReady = false;
    updater::Staged ready;
    updater::Result applied;
};
std::mutex g_mx;
State g_state;
std::atomic<bool> g_working{false};
oasis::ChannelId g_channel = 0;
std::once_flag g_selfOnce;
updater::Signer g_self;

const updater::Signer& Self() {
    std::call_once(g_selfOnce, [] {
        g_self = updater::ReadSigner(ExePath());
        if (g_self.subject.empty()) LOG_INFO("[update] Melange.exe is unsigned (a developer build): signatures of downloads are not compared");
        else LOG_INFO("[update] Melange.exe signed by %s (%s)%s", g_self.subject.c_str(), g_self.thumbprint.c_str(), g_self.valid ? "" : ", not valid here");
    });
    return g_self;
}

std::string StatusJsonLocked() {
    const State& s = g_state;
    jsonmini::Obj o;
    o.Str("current", MELANGE_VERSION).Str("phase", s.phase).Bool("auto", s.autoCheck);
    if (!s.latest.empty()) o.Str("latest", s.latest);
    if (!s.htmlUrl.empty()) o.Str("htmlUrl", s.htmlUrl);
    if (s.phase == "downloading") o.Raw("progress", jsonmini::Obj().UInt("got", s.got).UInt("total", s.total).End());
    if (!s.error.empty()) o.Str("error", s.error);
    if (!s.lastCheck.empty()) o.Str("lastCheck", s.lastCheck);
    if (s.applied.present) o.Raw("applied", updater::ResultJson(s.applied));
    return o.End();
}

std::string StatusJson() {
    std::lock_guard lk(g_mx);
    return StatusJsonLocked();
}

void Publish() {
    if (g_channel && oasis::HasSubscribers(g_channel)) oasis::Publish(g_channel, "{\"status\":" + StatusJson() + "}");
}

void OnSub(oasis::ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (subscribed) oasis::PublishTo(ch, client, "{\"status\":" + StatusJson() + "}");
}

void Set(const std::function<void(State&)>& fn) {
    {
        std::lock_guard lk(g_mx);
        fn(g_state);
    }
    Publish();
}

void Work(bool manual) {
    const std::string now = NowIsoUtc();
    app::UpdateSettings([&](Settings& s) { s.lastUpdateCheck = now; });
    Set([&](State& s) {
        s.lastCheck = now;
        s.error.clear();
        if (!s.haveReady) s.phase = "checking";
    });
    updater::Source src;
    src.userAgent = std::string("Melange/") + MELANGE_VERSION;
    update::Release rel;
    std::string err;
    if (!updater::FetchLatest(src, &rel, &err)) {
        LOG_INFO("[update] check failed: %s", err.c_str());
        Set([&](State& s) {
            s.phase = s.haveReady ? "ready" : manual ? "error" : "idle";
            if (manual) s.error = err;
        });
        return;
    }
    LOG_INFO("[update] latest release %s (this is %s)", rel.version.c_str(), MELANGE_VERSION);
    if (update::CompareVersions(rel.version, MELANGE_VERSION) <= 0) {
        // A staged version newer than the latest release was withdrawn: drop it.
        updater::Clean(updater::Root(), "", ExePath());
        Set([&](State& s) {
            s.phase = "current";
            s.latest = rel.version;
            s.htmlUrl = rel.htmlUrl;
            s.haveReady = false;
        });
        return;
    }
    bool already = false;
    Set([&](State& s) {
        already = s.haveReady && s.ready.version == rel.version;
        if (!already) return;
        s.phase = "ready";
        if (!rel.htmlUrl.empty()) s.htmlUrl = rel.htmlUrl;
    });
    if (already) return;
    Set([&](State& s) {
        s.phase = "downloading";
        s.latest = rel.version;
        s.htmlUrl = rel.htmlUrl;
        s.got = s.total = 0;
    });
    ULONGLONG lastPublish = 0;
    updater::Staged staged;
    const bool ok = updater::Download(src, rel, updater::Root(), Self(), [&](uint64_t got, uint64_t total) {
        const ULONGLONG t = GetTickCount64();
        {
            std::lock_guard lk(g_mx);
            g_state.got = got;
            g_state.total = total;
        }
        if (t - lastPublish >= 250 || got == total) {
            lastPublish = t;
            Publish();
        }
    }, &staged, &err);
    if (!ok) {
        LOG_WARN("[update] Melange %s was not staged: %s", rel.version.c_str(), err.c_str());
        Set([&](State& s) {
            s.phase = s.haveReady ? "ready" : manual ? "error" : "idle";
            if (s.haveReady) {   // still offer the one already staged
                s.latest = s.ready.version;
                s.htmlUrl = s.ready.htmlUrl;
            }
            if (manual) s.error = "Melange " + rel.version + " could not be downloaded: " + err;
        });
        return;
    }
    LOG_INFO("[update] Melange %s is ready in %ls", staged.version.c_str(), staged.dir.c_str());
    updater::Clean(updater::Root(), staged.version, ExePath());
    Set([&](State& s) {
        s.phase = "ready";
        s.haveReady = true;
        s.ready = staged;
        s.latest = staged.version;
    });
}

bool StartWork(bool manual) {
    bool expected = false;
    if (!g_working.compare_exchange_strong(expected, true)) return false;
    std::thread([manual] {
        GuardedThreadBody("update", [&] { Work(manual); });
        g_working = false;
    }).detach();
    return true;
}

// ---------------------------------------------------------------- methods
void StatusMethod(const Call&, Result& r, void*) { r.json = StatusJson(); }

void CheckMethod(const Call&, Result& r, void*) {
    StartWork(true);
    r.json = StatusJson();
}

// {on}: the launcher's check at start and the game's daily one ([Update] CheckInGame) together. A running game
// picks the ini change up at its next start.
void SetAutoMethod(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::Params(c, r, &p)) return;
    const json::Value* on = p.Get("on");
    if (!on || !on->IsBool()) return rpc::Fail(r, -32602, "expected {on}");
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return rpc::Fail(r, -32002, app::BusyMessage());
    const bool v = on->boolean;
    LOG_INFO("[update] automatic checks %s", v ? "on" : "off");
    app::UpdateSettings([&](Settings& s) { s.autoUpdate = v; });
    Set([&](State& s) { s.autoCheck = v; });
    SyncInGameCheck();
    lk.unlock();
    if (v) StartWork(false);
    r.json = StatusJson();
}

void ApplyMethod(const Call&, Result& r, void*) {
    updater::Staged ready;
    {
        std::lock_guard lk(g_mx);
        if (!g_state.haveReady) return rpc::Fail(r, -32000, "No update is ready yet.");
        ready = g_state.ready;
    }
    const std::wstring game = app::GameDir();
    if (!game.empty() && setup::GameRunning(game)) return rpc::Fail(r, -32000, "Close the game to update.");
    if (rpc::ImportRunning()) return rpc::Fail(r, -32002, "Wait for the import to finish, then update.");
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return rpc::Fail(r, -32002, app::BusyMessage());
    std::string why;
    if (!updater::VerifyPayload(ready.payload, ready.version, Self(), &why)) {
        LOG_WARN("[update] the staged %s no longer verifies: %s", ready.version.c_str(), why.c_str());
        updater::Clean(updater::Root(), "", ExePath());
        Set([](State& s) {
            s.haveReady = false;
            s.phase = "idle";
        });
        StartWork(false);
        return rpc::Fail(r, -32000, "The downloaded update didn't check out (" + why + "). Melange is downloading it again.");
    }
    updater::ApplyArgs a;
    a.from = ExeDir();
    a.game = game;
    a.pid = GetCurrentProcessId();
    const std::wstring exe = ready.payload + L"\\Melange.exe";
    std::wstring cmd = updater::ApplyCommandLine(exe, a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ready.payload.c_str(), &si, &pi))
        return rpc::Fail(r, -32000, "Could not start the update: " + Win32Message(GetLastError()));
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOG_INFO("[update] handing over to %ls", exe.c_str());
    window::QuitSoon();
    r.json = "{}";
}

// ---------------------------------------------------------------- --apply-update
std::wstring ElevatedResultPath() { return updater::Root() + L"\\result-elevated.json"; }

// Runs this Melange.exe again as administrator for the same apply, and waits for it.
bool RunElevated(const updater::ApplyArgs& args, updater::Result* out) {
    updater::ApplyArgs child = args;
    child.elevated = true;
    child.result = ElevatedResultPath();
    DeleteFileW(child.result.c_str());
    const std::wstring exe = ExePath(), params = updater::ApplyCommandLine(L"", child);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        LOG_WARN("[update] could not start as administrator (%lu)", GetLastError());
        return false;
    }
    WaitForSingleObject(sei.hProcess, 10 * 60 * 1000);
    CloseHandle(sei.hProcess);
    return updater::TakeResult(child.result, out);
}

void Relaunch(const std::wstring& exe, const std::wstring& game, const std::wstring& savedGame) {
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!game.empty() && PathKey(game) != PathKey(savedGame)) cmd += L" --game \"" + game + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, Parent(exe).c_str(), &si, &pi)) {
        LOG_WARN("[update] could not start %ls: %s", exe.c_str(), Win32Message(GetLastError()).c_str());
        return;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}
}  // namespace

void Start(bool autoCheck) {
    updater::Result applied;
    if (updater::TakeResult(updater::ResultPath(), &applied)) {
        LOG_INFO("[update] last apply: %s %s%s%s", applied.ok ? "updated to" : "failed for", applied.version.c_str(),
                 applied.message.empty() ? "" : ": ", applied.message.c_str());
        if (applied.ok && applied.version == MELANGE_VERSION) OnMelangeUpdated(app::GameDir());
    }
    updater::DeleteOldExe(ExeDir());
    updater::DeleteOldExe(app::GameDir());
    updater::Staged ready;
    const bool haveReady = updater::FindReady(updater::Root(), MELANGE_VERSION, &ready);
    updater::Clean(updater::Root(), haveReady ? ready.version : "", ExePath());
    const Settings settings = app::GetSettings();
    {
        std::lock_guard lk(g_mx);
        g_state.applied = applied;
        g_state.lastCheck = settings.lastUpdateCheck;
        g_state.autoCheck = settings.autoUpdate;
        if (haveReady) {
            g_state.haveReady = true;
            g_state.ready = ready;
            g_state.phase = "ready";
            g_state.latest = ready.version;
            g_state.htmlUrl = ready.htmlUrl;
        }
    }
    // An install or update since the last start may have brought the template's CheckInGame=1 back.
    SyncInGameCheck();
    if (autoCheck && settings.autoUpdate) StartWork(false);
    else if (autoCheck) LOG_INFO("[update] automatic checks are off (Settings › Updates)");
}

void SyncInGameCheck() {
    bool changed = false;
    std::string err;
    if (!updater::SyncInGameCheck(app::GameDir(), app::GetSettings().autoUpdate, &changed, &err))
        LOG_WARN("[update] the in-game check setting was not written: %s", err.c_str());
}

void Install() {
    using oasis::kRpcMutating;
    using oasis::kRpcServerThread;
    g_channel = oasis::AddChannel("update");
    oasis::OnSubscribe(g_channel, &OnSub, nullptr);
    oasis::AddMethod("update.status", &StatusMethod, nullptr, kRpcServerThread);
    oasis::AddMethod("update.check", &CheckMethod, nullptr, kRpcServerThread);
    oasis::AddMethod("update.apply", &ApplyMethod, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("update.setAuto", &SetAutoMethod, nullptr, kRpcServerThread | kRpcMutating);
}

int RunApply(const updater::ApplyArgs& args) {
    LOG_INFO("[update] applying Melange %s from %ls (replacing pid %lu from %ls)%s", MELANGE_VERSION, ExeDir().c_str(), args.pid,
             args.from.c_str(), args.elevated ? " as administrator" : "");
    HANDLE mutex = nullptr;
    updater::Result res;
    res.present = true;
    res.version = MELANGE_VERSION;
    bool waited = true;
    if (!args.elevated) {
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, args.pid)) {
            waited = WaitForSingleObject(p, 30000) == WAIT_OBJECT_0;
            CloseHandle(p);
        }
        // Holding the launcher's single-instance mutex keeps a Melange.exe started meanwhile from racing the apply.
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\Melange-Launcher");
        if (mutex) WaitForSingleObject(mutex, 15000);
    }
    Settings saved;
    LoadSettings(SettingsPath(), &saved);
    const std::wstring game = !args.game.empty() ? args.game : saved.gameDir;
    updater::ApplyOutcome o;
    if (!waited) {
        o.message = "The running Melange didn't close, so nothing was changed.";
    } else {
        setup::Context ctx;
        ctx.gameDir = game;
        ctx.payloadDir = ExeDir();
        ctx.selfExe = ExePath();
        ctx.version = MELANGE_VERSION;
        ctx.logsDir = game.empty() ? std::wstring() : oasis::standalone::LogsDir(game);
        ctx.protect = setup::ProtectFromEnv();
        ctx.storeOf = [](const std::wstring& dir) { return setup::StoreOf(dir, setup::SystemRegistry()); };
        o = updater::ApplyStaged(ctx, args.from);
        if (!o.ok && o.needElevation && !args.elevated && !app::Elevated()) {
            LOG_INFO("[update] %s; asking Windows to run the update as administrator", o.message.c_str());
            updater::Result child;
            if (RunElevated(args, &child)) {
                o.ok = child.ok;
                o.message = child.message;
                o.warnings = child.warnings;
                o.gameUpdated = child.gameUpdated;
            } else {
                o.message = "Windows didn't let Melange update its files, and didn't start it as administrator. " + o.message;
            }
        }
    }
    res.ok = o.ok;
    res.gameUpdated = o.gameUpdated;
    res.message = o.ok ? std::string() : o.message;
    res.warnings = o.warnings;
    res.at = NowIsoUtc();
    if (o.ok) LOG_INFO("[update] done%s%s", o.backupId.empty() ? "" : "; backup ", o.backupId.c_str());
    else LOG_WARN("[update] failed: %s", o.message.c_str());
    for (const auto& w : o.warnings) LOG_WARN("[update] %s", w.c_str());
    if (args.elevated) {
        updater::WriteResult(args.result.empty() ? updater::ResultPath() : args.result, res);
        return o.ok ? 0 : 1;
    }
    updater::WriteResult(updater::ResultPath(), res);
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    // Back to a normal start: the updated copy in the game folder, else (and after a failure) the one that ran before.
    const std::wstring inGame = game.empty() ? std::wstring() : game + L"\\Melange.exe";
    const std::wstring origin = args.from + L"\\Melange.exe";
    std::wstring next = o.ok && o.gameUpdated && FileExists(inGame) ? inGame : FileExists(origin) ? origin : inGame;
    if (!next.empty() && FileExists(next)) Relaunch(next, game, saved.gameDir);
    return o.ok ? 0 : 1;
}
}  // namespace updatehost
}  // namespace melange::launcher
