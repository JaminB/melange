#pragma once
#include <string>
namespace wf::exporter {
struct Options {
    int sessions = 3;              // newest N session folders
    bool includeDumps = true;      // minidumps from DataDir (newest 3)
    bool includeFullDumps = false; // FullMemoryDumps=1 dumps can be hundreds of MB and hold process memory
    bool redactUserPaths = true;   // replace the Windows user name in paths/text with %USERNAME%
};
Options DefaultOptions();  // from [LogExport] in WUMFix.ini
// Opens the Save-As dialog on a dedicated worker thread and writes the zip there. Non-blocking; returns false
// if an export is already running. Callable from any thread (hotkey, overlay button).
bool RequestSaveAs();
// Synchronous export without UI (tests, automation "savelogs <path>"). Must NOT be called on the main thread.
bool ExportTo(const std::wstring& zipPath, const Options& opt, std::string* error);
enum class State { Idle, Dialog, Writing, Done, Failed, Cancelled };
State Status(std::wstring* lastPath = nullptr, std::string* lastError = nullptr);
}
