#pragma once
#include <string>

#include "launcher/updater.h"

// Melange.exe's side of updating itself: the check at start and on request, the background download, the "update"
// channel, update.* methods, and the --apply-update run of a downloaded Melange.exe.
namespace melange::launcher {
// Called once, by the first launcher start after an update was applied successfully.
void OnMelangeUpdated(const std::wstring& gameDir);

namespace updatehost {
// Picks up the last apply's result and any update already downloaded; `autoCheck` also looks at GitHub now.
void Start(bool autoCheck);
void Install();   // update.status, update.check, update.apply and the "update" channel
// The new Melange.exe, started with --apply-update by the old one. Returns the process exit code.
int RunApply(const updater::ApplyArgs& args);
}  // namespace updatehost
}  // namespace melange::launcher
