#pragma once
#include <functional>
#include <string>
#include <vector>

#include "launcher/setup/dll_id.h"
#include "launcher/setup/exe_check.h"

// Install, repair, uninstall and restore Melange in a game folder: every change is planned first, staged, backed
// up and committed by rename, and rolled back on failure. UI-free; the self-test drives it on a fake folder.
namespace melange::launcher::setup {
using MoveFn = std::function<unsigned long(const std::wstring& from, const std::wstring& to)>;
unsigned long DefaultMove(const std::wstring& from, const std::wstring& to);

struct Context {
    std::wstring gameDir;
    std::wstring payloadDir;          // melange.asi, dinput8.dll, Melange.ini beside Melange.exe
    std::wstring selfExe;             // the Melange.exe to copy into the game folder ("" = none)
    std::string version;              // this release's Melange version
    std::wstring logsDir;             // where the game writes session logs ("" = unknown)
    const std::vector<Profile>* profiles = nullptr;
    std::vector<std::wstring> protect;   // MELANGE_PROTECT
    std::function<bool(const std::wstring&)> running;   // default: GameRunning
    std::function<bool(const std::wstring&)> loaded;    // default: MelangeLoaded
    std::function<std::string(const std::wstring&)> storeOf;   // steam | gog | unknown (default: unknown)
    MoveFn move;                      // default: DefaultMove
    std::function<void(int step, int of, const std::string& label)> progress;
};
std::vector<std::wstring> ProtectFromEnv();   // MELANGE_PROTECT, ';'-separated

struct Backup {
    std::string id, created, action;
    struct File { std::string path, op, sha256, kind, description; };
    std::vector<File> files;
};
struct InstallRecord {
    bool present = false;
    std::string melange, installedAt, loader, loaderSha256;
};
struct Payload {
    bool ok = false, fromGameFolder = false;
    std::string version, asiSha, ualSha;
    std::vector<std::string> missing;
};
struct Status {
    GameCheck game;
    bool haveGame = false, running = false, melangeLoaded = false;
    std::string loaderState = "none";   // none | ual | other
    bool haveLoader = false;
    DllInfo loader;
    std::vector<DllInfo> otherLoaders;
    std::string melangeState = "missing";   // missing | installed | disabled | older | newer | damaged
    std::string melangeVersion, melangePath, lastLoadAt, lastLoadVersion;
    std::vector<std::string> duplicates, legacy;
    bool iniPresent = false;
    int iniMissing = 0;
    Payload payload;
    std::vector<Backup> backups;
    InstallRecord install;
};
Status Inspect(const Context& ctx);
std::string StatusJson(const Status& s);

struct PlanRequest {
    std::string action = "install";   // install | repair | uninstall
    bool replaceLoader = false, allowDowngrade = false, removeData = false;
};
struct Step {
    std::string op;     // add | replace | remove | backup | merge | keep
    std::string path;   // relative to the game folder
    std::string detail;
};
struct Plan {
    std::string planId;
    std::vector<Step> steps;
    std::string needsChoice;   // "" | "loader"
    std::string refused;       // user copy, "" when the plan can run
    int code = 0;              // with refused: -32000, -32011 (payload missing)
    std::vector<std::string> missing;
};
Plan MakePlan(const Context& ctx, const PlanRequest& req);
std::string PlanJson(const Plan& p);

struct Outcome {
    bool ok = false;
    int code = 0;              // -32000 refused, -32010 access denied, -32011 payload missing, -32012 rolled back,
                               // -32013 plan changed, -32602 bad params
    std::string message, failedPath, backupId;
    unsigned long win32 = 0;
    std::vector<std::string> missing;
};
// Recomputes the plan and refuses when its id differs from `planId` ("" skips the check).
Outcome Apply(const Context& ctx, const PlanRequest& req, const std::string& planId);
Outcome Restore(const Context& ctx, const std::string& backupId);
Outcome DeleteBackup(const Context& ctx, const std::string& backupId);
Outcome SetMelangeEnabled(const Context& ctx, bool on);
std::vector<Backup> ListBackups(const std::wstring& gameDir);
// Refusal copy shared with other writers (plugins, store, ini): "" when writes to the folder are allowed now.
std::string WriteGate(const Context& ctx);
}  // namespace melange::launcher::setup
