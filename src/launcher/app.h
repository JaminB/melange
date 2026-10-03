#pragma once
#include <windows.h>

#include <functional>
#include <mutex>
#include <string>

#include "launcher/settings.h"
#include "launcher/setup/engine.h"

// Melange.exe's process-wide state: options, launcher.json, the chosen game folder, the setup channel.
namespace melange::launcher::app {
struct Options {
    std::wstring game;      // --game: this run only
    std::wstring webRoot;   // --web-root
    std::string resume;     // --resume
    bool browser = false, serve = false, devtools = false;
};
void Init(const Options& o);
const Options& Opts();
std::string Version();

std::wstring GameDir();                            // any thread; "" before a folder is chosen
void SetGameDir(const std::wstring& dir, bool save);
Settings GetSettings();
void UpdateSettings(const std::function<void(Settings&)>& fn);   // applies and saves launcher.json

setup::Context MakeContext();                      // for the current folder
std::string WriteGate();                           // "" when writes to the current folder are allowed now
std::string CachedGate();                          // the last poll's WriteGate (cheap)
std::mutex& Tx();                                  // one setup transaction at a time

void StartChannel();                               // the "setup" channel and its 2 s poll
void PublishStatus();                              // push the status now (after a change)
void PublishProgress(const std::string& action, int step, int of, const std::string& label);
std::string StatusJson();

// A long batch outside the normal setup.apply/restore call (today: recommended.apply installing the first-run
// plugin set) that holds Tx() for its whole run. Set true before the batch starts and false when it ends so
// setup.status (and anyone polling it) can show it, and so a -32002 while it runs says what is busy instead of
// a generic "try again". SetBatchBusy also publishes progress and status on the "setup" channel.
void SetBatchBusy(bool active, const std::string& action, int step, int of, const std::string& label);
bool BatchBusy(std::string* label = nullptr);
std::string BusyMessage();   // -32002 text: specific while a batch is busy, generic otherwise

// The UI thread (window mode): the window, the theme, and a queue for work posted from other threads.
void SetWindow(HWND hwnd);
HWND Window();
bool WebView();
void SetWebView(bool on);
bool Elevated();
}  // namespace melange::launcher::app
