#pragma once
#include <windows.h>

#include <string>

// The native side of Melange.exe: the main window hosting the Oasis page in WebView2, the fallback when WebView2 is
// missing, and the small window of --browser mode. Everything here runs on the UI thread unless marked.
namespace melange::launcher::window {
// The main window. Falls back to RunBrowser after asking, when WebView2 cannot be used. Returns the exit code.
int Run(HINSTANCE inst, const std::string& url, int port);
// The page in the default browser and a small "Melange is open in your browser" window.
int RunBrowser(HINSTANCE inst, const std::string& url);
// --serve: no window; pumps until the process is stopped.
int RunHeadless();

void ApplyTheme(const std::string& theme);   // any thread
void QuitSoon();                             // any thread: close shortly (after an elevated restart)
bool DarkTheme(const std::string& theme);    // the effective theme: "system" follows Windows
}  // namespace melange::launcher::window
