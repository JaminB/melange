// setup.*: find and check the game, plan and apply install/repair/uninstall, backups, restore vanilla. The "W"
// methods run on the calling server thread; mutations hold the setup transaction lock (another one running is -32002).
#include <windows.h>
#include <objbase.h>

#include <shellapi.h>
#include <shobjidl.h>

#include <thread>

#include "core/log.h"
#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/setup/detect.h"
#include "launcher/setup/engine.h"
#include "launcher/setup/running.h"
#include "launcher/setup/vanilla.h"
#include "launcher/store_host.h"
#include "launcher/update_host.h"
#include "launcher/updater.h"
#include "launcher/util.h"
#include "launcher/window.h"
#include "oasis/standalone/register.h"
#include "store/store.h"
#include "tools/json_mini.h"

namespace melange::launcher::rpc {
namespace {
using oasis::Call;
using oasis::Result;

setup::GameCheck FullCheck(const std::wstring& path) {
    setup::GameCheck c = setup::CheckExe(path);
    c.store = setup::StoreOf(c.path, setup::SystemRegistry());
    c.running = !c.path.empty() && setup::GameRunning(c.path);
    c.writable = setup::CanWrite(c.path);
    return c;
}

void SetError(Result& r, const setup::Outcome& o) {
    std::string data;
    if (o.code == -32011) {
        jsonmini::Arr m;
        for (const auto& x : o.missing) m.Str(x);
        data = jsonmini::Obj().Raw("missing", m.End()).End();
    } else if (o.code == -32012 || o.code == -32010) {
        data = jsonmini::Obj().Str("path", o.failedPath).UInt("win32", o.win32).Str("message", Win32Message(o.win32)).End();
    }
    Fail(r, o.code ? o.code : -32000, o.message, data);
}

bool ReadRequest(const json::Value& p, Result& r, setup::PlanRequest* rq) {
    rq->action = Str(p, "action");
    if (rq->action != "install" && rq->action != "repair" && rq->action != "uninstall") {
        Fail(r, -32602, "action must be install, repair or uninstall");
        return false;
    }
    rq->replaceLoader = Bool(p, "replaceLoader");
    rq->allowDowngrade = Bool(p, "allowDowngrade");
    rq->removeData = Bool(p, "removeData");
    return true;
}

std::string ApplyResult(const setup::Outcome& o) {
    jsonmini::Obj res;
    res.Bool("ok", true);
    if (!o.backupId.empty()) res.Str("backupId", o.backupId);
    res.Raw("status", app::StatusJson());
    return res.End();
}

// ---------------------------------------------------------------- read-only
void Detect(const Call&, Result& r, void*) {
    setup::DetectInput in;
    const std::wstring current = app::GameDir();
    in.saved = !current.empty() ? current : app::GetSettings().gameDir;
    in.selfDir = ExeDir();
    jsonmini::Arr arr;
    for (auto& c : setup::Detect(in)) {
        c.check.running = setup::GameRunning(c.path);
        c.check.writable = setup::CanWrite(c.path);
        arr.Raw(setup::CandidateJson(c));
    }
    r.json = jsonmini::Obj().Raw("candidates", arr.End()).End();
}

void Validate(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string path = Str(p, "path");
    if (path.empty()) return Fail(r, -32602, "expected {path}");
    r.json = setup::GameCheckJson(FullCheck(Widen(path)));
}

void Status(const Call&, Result& r, void*) { r.json = app::StatusJson(); }

void PlanMethod(const Call& c, Result& r, void*) {
    json::Value p;
    setup::PlanRequest rq;
    if (!Params(c, r, &p) || !ReadRequest(p, r, &rq)) return;
    if (app::GameDir().empty()) return Fail(r, -32000, "Choose your game folder first.");
    r.json = setup::PlanJson(setup::MakePlan(app::MakeContext(), rq));
}

// ---------------------------------------------------------------- mutations
void Select(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string path = Str(p, "path");
    if (path.empty()) return Fail(r, -32602, "expected {path, save}");
    const setup::GameCheck check = FullCheck(Widen(path));
    if (check.verdict != setup::Verdict::Ok) {
        const char* why = check.verdict == setup::Verdict::WrongBuild ? "This version of the game isn't supported. Melange only works with the Steam/GOG release, build #1077."
                          : check.verdict == setup::Verdict::NoExe    ? "WormsMayhem.exe isn't in this folder."
                          : check.verdict == setup::Verdict::NotFound ? "That folder no longer exists."
                                                                       : "We couldn't read WormsMayhem.exe.";
        return Fail(r, -32000, why, jsonmini::Obj().Raw("check", setup::GameCheckJson(check)).End());
    }
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    app::SetGameDir(check.path, Bool(p, "save"));
    storehost::Sync();
    updatehost::SyncInGameCheck();
    oasis::standalone::RegisterLevels(check.path);
    r.json = app::StatusJson();
}

void Apply(const Call& c, Result& r, void*) {
    json::Value p;
    setup::PlanRequest rq;
    if (!Params(c, r, &p) || !ReadRequest(p, r, &rq)) return;
    const std::string planId = Str(p, "planId");
    if (planId.empty()) return Fail(r, -32602, "expected planId");
    if (app::GameDir().empty()) return Fail(r, -32000, "Choose your game folder first.");
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    setup::Context ctx = app::MakeContext();
    ctx.progress = [action = rq.action](int step, int of, const std::string& label) { app::PublishProgress(action, step, of, label); };
    const setup::Outcome o = setup::Apply(ctx, rq, planId);
    if (o.ok && rq.action != "uninstall") updatehost::SyncInGameCheck();   // a new Melange.ini has the template's CheckInGame=1
    lk.unlock();
    if (!o.ok) {
        app::PublishStatus();
        return SetError(r, o);
    }
    if (rq.action != "uninstall" && PathKey(app::GetSettings().gameDir) == PathKey(ctx.gameDir))
        app::UpdateSettings([](Settings& s) { s.firstRunDone = true; });
    app::PublishStatus();
    r.json = ApplyResult(o);
}

void RestoreMethod(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string id = Str(p, "backupId");
    if (id.empty()) return Fail(r, -32602, "expected {backupId}");
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    setup::Context ctx = app::MakeContext();
    ctx.progress = [](int step, int of, const std::string& label) { app::PublishProgress("restore", step, of, label); };
    const setup::Outcome o = setup::Restore(ctx, id);
    if (o.ok) updatehost::SyncInGameCheck();
    lk.unlock();
    app::PublishStatus();
    if (!o.ok) return SetError(r, o);
    r.json = ApplyResult(o);
}

void DeleteBackupMethod(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string id = Str(p, "backupId");
    if (id.empty()) return Fail(r, -32602, "expected {backupId}");
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    const setup::Outcome o = setup::DeleteBackup(app::MakeContext(), id);
    lk.unlock();
    app::PublishStatus();
    if (!o.ok) return SetError(r, o);
    r.json = "{}";
}

void SetEnabled(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const json::Value* on = p.Get("on");
    if (!on || !on->IsBool()) return Fail(r, -32602, "expected {on}");
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    const setup::Outcome o = setup::SetMelangeEnabled(app::MakeContext(), on->boolean);
    lk.unlock();
    app::PublishStatus();
    if (!o.ok) return SetError(r, o);
    r.json = app::StatusJson();
}

// ---------------------------------------------------------------- restore vanilla
void VanillaPlanMethod(const Call&, Result& r, void*) {
    if (app::GameDir().empty()) return Fail(r, -32000, "Choose your game folder first.");
    setup::VanillaContext ctx;
    ctx.base = app::MakeContext();
    r.json = setup::VanillaPlanJson(setup::MakeVanillaPlan(ctx));
}

// Deletes everything that isn't the stock game (no backup), then forgets the game so the next start is a first run.
// Overwritten stock files are put back by Steam's verify, started here; other stores get copy in the result.
void VanillaApplyMethod(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string planId = Str(p, "planId");
    if (planId.empty()) return Fail(r, -32602, "expected planId");
    if (app::GameDir().empty()) return Fail(r, -32000, "Choose your game folder first.");
    if (store::GetStatus().busy || ImportRunning()) return Fail(r, -32002, "Melange is busy with your plugins. Try again in a moment.");
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    setup::VanillaContext ctx;
    ctx.base = app::MakeContext();
    ctx.base.progress = [](int step, int of, const std::string& label) { app::PublishProgress("vanilla", step, of, label); };
    setup::VanillaOutcome o = setup::ApplyVanilla(ctx, planId);
    lk.unlock();
    if (!o.outcome.ok) {
        app::PublishStatus();
        return SetError(r, o.outcome);
    }
    app::DeleteOnExit(o.pending);
    app::ForgetGame();
    updater::Clean(updater::Root(), "", ExePath());
    DeleteFileW(updater::ResultPath().c_str());
    if (o.plan.verify && o.plan.store == "steam") {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        o.verifyStarted = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"steam://validate/70600", nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        if (SUCCEEDED(init)) CoUninitialize();
        LOG_INFO("[vanilla] Steam verify of the game files %s", o.verifyStarted ? "started" : "could not be started");
    } else if (o.plan.verify) {
        LOG_INFO("[vanilla] stock files need restoring; not a Steam install (%s), the user is asked to verify or reinstall", o.plan.store.c_str());
    }
    r.json = setup::VanillaOutcomeJson(o);
}

// ---------------------------------------------------------------- main thread
void Browse(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    std::wstring start = Widen(Str(p, "start"));
    if (start.empty()) start = app::GameDir();
    if (start.empty())
        for (const auto& lib : setup::SteamLibraries(setup::SystemRegistry()))
            if (DirExists(lib + L"\\steamapps\\common")) {
                start = lib + L"\\steamapps\\common";
                break;
            }
    std::wstring picked;
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Choose the folder that contains WormsMayhem.exe");
        IShellItem* folder = nullptr;
        if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
        HWND owner = app::Window();
        if (owner) SetForegroundWindow(owner);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    picked = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    if (picked.empty()) {
        r.json = "{\"path\":null}";
        return;
    }
    jsonmini::Obj o;
    o.Str("path", Narrow(FullPath(picked)));
    const std::wstring child = setup::ChildWithGame(picked);
    if (!child.empty()) o.Str("hint", "child").Str("child", Narrow(child));
    r.json = o.End();
}

void Elevate(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string resume = Str(p, "resume");
    for (char ch : resume)
        if (!(isalnum(static_cast<unsigned char>(ch)) || ch == ':' || ch == '-' || ch == '_' || ch == '.' || ch == ','))
            return Fail(r, -32602, "bad resume action");
    std::wstring args;
    const std::wstring game = app::GameDir();
    if (!game.empty()) args += L"--game \"" + game + L"\" ";
    if (!resume.empty()) args += L"--resume " + Widen(resume) + L" ";
    if (app::Opts().browser) args += L"--browser ";
    const std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.hwnd = app::Window();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        return Fail(r, -32000, e == ERROR_CANCELLED ? "Windows didn't start Melange as administrator." : "Could not restart: " + Win32Message(e));
    }
    window::QuitSoon();
    r.json = "{}";
}
}  // namespace

void InstallSetup() {
    using oasis::kRpcMutating;
    using oasis::kRpcServerThread;
    oasis::AddMethod("setup.detect", &Detect, nullptr, kRpcServerThread);
    oasis::AddMethod("setup.validate", &Validate, nullptr, kRpcServerThread);
    oasis::AddMethod("setup.status", &Status, nullptr, kRpcServerThread);
    oasis::AddMethod("setup.plan", &PlanMethod, nullptr, kRpcServerThread);
    oasis::AddMethod("setup.select", &Select, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("setup.apply", &Apply, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("setup.restore", &RestoreMethod, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("setup.deleteBackup", &DeleteBackupMethod, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("setup.setMelangeEnabled", &SetEnabled, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("setup.vanillaPlan", &VanillaPlanMethod, nullptr, kRpcServerThread);
    oasis::AddMethod("setup.vanillaApply", &VanillaApplyMethod, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("setup.browse", &Browse, nullptr, oasis::kRpcNone);
    oasis::AddMethod("setup.elevate", &Elevate, nullptr, oasis::kRpcNone);
}
}  // namespace melange::launcher::rpc
