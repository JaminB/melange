#pragma once
// The log export without the game: picks the files, redacts them and writes the zip. Shared by the in-game LogExport
// module (tools/log_export.cpp) and Melange.exe (launcher.exportLogs), so both apply exactly the same redaction.
// What only the running game knows (live system.json, the GPU report, the installed modules) comes in as Providers.
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "melange/export.h"

namespace melange::exporter::core {

// LastGame: one game process -- its session folder, its replays and desync bundles (by the pid in their names),
// the dumps and engine logs from its time window. RecentSessions: "Save logs as...", the newest Options::sessions
// folders and the newest replay/bundle/dumps, whichever game they came from.
enum class Scope { LastGame, RecentSessions };

struct Sources {
    // Folders holding session folders (YYYY-MM-DD_HH-MM-SS_pid<N>), searched together: the configured or Documents
    // one first, then the <game>\Melange\logs fallback. Duplicates and missing folders are fine.
    std::vector<std::wstring> sessionRoots;
    // <folder of melange.asi>\Melange candidates (Melange.log, dumps\). The one with the newest Melange.log is used.
    std::vector<std::wstring> dataDirs;
    // More dumps folders besides the chosen data dir's dumps\: Documents\Melange\dumps, where the game writes a dump
    // when its own folder is not writable (core/dump_paths.h). Duplicates and missing folders are fine.
    std::vector<std::wstring> dumpDirs;
    std::wstring gameDir;         // engine XOM/Net logs, *.ini, Mods\ (spice.json, thumper-state.json, .store)
    std::wstring replaysDir;       // Documents\Melange\replays
    std::wstring launcherLogDir;   // %LOCALAPPDATA%\Melange (launcher.log, launcher.1.log)
    // In-game: this process's session folder and pid, so LastGame means "this game" and the session is still
    // live (its window runs to now). Empty/0 in Melange.exe: the newest session folder on disk is the last game.
    std::wstring currentSessionDir;
    uint32_t currentPid = 0;
};

struct Providers {
    std::function<void()> beforeCollect;         // e.g. jlog::Flush, so the session file is on disk
    std::function<std::string()> systemJson;     // default: sysinfo::OfflineJson(gameDir)
    std::function<std::string()> pluginsJson;    // default: sysinfo::PluginsJsonIn(gameDir)
    std::function<std::string()> modulesJson;    // mods/modules.json; omitted when not set
    std::function<std::string()> gpuCompatJson;  // gpu/compat.json; omitted when not set
    std::function<std::string()> gpuCompatText;  // gpu/compat.txt
};

struct Request {
    Scope scope = Scope::LastGame;
    Options opt;
    Sources src;
    Providers prov;
    std::string producer = "game";  // "game" or "launcher", recorded in manifest.json
};

struct Result {
    std::wstring path;
    uint64_t bytes = 0;      // zip size on disk
    size_t entries = 0;      // files in the zip besides manifest.json and README.txt
    std::string sessionId;   // LastGame: the chosen session folder's name, "" when none was found
    uint32_t pid = 0;        // LastGame: the game process the replays were matched by, 0 when unknown
    std::string error;       // set when Export returns false
};

// Synchronous; reads and deflates everything in memory, so call it off any UI/render thread. Overwrites zipPath.
bool Export(const std::wstring& zipPath, const Request& rq, Result* out);

// -- Selection helpers (the self-test drives these directly) ------------------------------------------------------
// "YYYY-MM-DD_HH-MM-SS_pid<N>" -> its pid and local start time as UTC FILETIME ticks.
bool ParseSessionName(std::wstring_view name, uint32_t* pid, uint64_t* startUtc);
// "wsr-YYYYMMDD-HHMMSS-p<pid>-..." and "desync-YYYYMMDD-HHMMSS-p<pid>-..." -> pid and UTC FILETIME ticks.
bool ParseReplayName(std::wstring_view name, uint32_t* pid, uint64_t* whenUtc);

struct SessionDir {
    std::wstring path;
    std::string id;       // the folder name
    uint32_t pid = 0;
    uint64_t startUtc = 0;
};
// Every session folder under `roots`, newest first; a folder reachable from two roots is listed once.
std::vector<SessionDir> FindSessions(const std::vector<std::wstring>& roots);

// -- One-click destination --------------------------------------------------------------------------------------
enum class Folder { Documents, Desktop, LocalAppData, Profile };
std::wstring KnownFolder(Folder f);   // "" if Windows can't resolve it
std::wstring DefaultZipName();        // Melange-logs-YYYYMMDD-HHMMSS.zip (local time)
// The Desktop when it is writable, else Documents\Melange\exports (created); a fresh file name in it.
// *onDesktop says which.
std::wstring OneClickPath(bool* onDesktop);
// Opens Explorer on the file's folder with the file selected. Initialises COM for the call if needed.
bool RevealInExplorer(const std::wstring& path);
}  // namespace melange::exporter::core
