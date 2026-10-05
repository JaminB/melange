#pragma once
#include <string>
namespace melange::exporter {
struct Options {
    int sessions = 3;              // newest N session folders ("Save logs as..." only)
    bool includeDumps = true;      // minidumps from DataDir (newest 3; for the last game, from its time window)
    bool includeFullDumps = false; // FullMemoryDumps=1 dumps can be hundreds of MB and hold process memory
    bool redactUserPaths = true;   // replace the Windows user name in paths/text with %USERNAME%
};
Options DefaultOptions();  // from [LogExport] in Melange.ini
// One click, no dialog: this game's logs (its session, every replay and desync bundle it wrote, dumps from it) go
// to the Desktop (else Documents\Melange\exports) as Melange-logs-<stamp>.zip on a worker thread. A toast shows the
// path; windowed, Explorer also opens with the zip selected. Non-blocking; false if an export is already running.
bool RequestExportLastGame();
// Opens the Save-As dialog on a dedicated worker thread and writes the zip there (the newest Options::sessions
// sessions). Non-blocking; returns false if an export is already running. Callable from any thread (hotkey, overlay).
bool RequestSaveAs();
// Synchronous export without UI (tests, automation "savelogs <path>"). Must NOT be called on the main thread.
bool ExportTo(const std::wstring& zipPath, const Options& opt, std::string* error);
// As ExportTo, but only this game, as RequestExportLastGame picks it (automation "savelogs-last [path]").
bool ExportLastGameTo(const std::wstring& zipPath, const Options& opt, std::string* error);
enum class State { Idle, Dialog, Writing, Done, Failed, Cancelled };
State Status(std::wstring* lastPath = nullptr, std::string* lastError = nullptr);
}
