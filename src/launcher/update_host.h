#pragma once
#include <string>

#include "launcher/updater.h"

// Melange.exe's side of updating itself: the check at start and on request, the background download, the "update"
// channel, update.* methods, and the --apply-update run of a downloaded Melange.exe.
namespace melange::launcher {
// Called once, by the first launcher start after an update was applied successfully.
void OnMelangeUpdated(const std::wstring& gameDir);

namespace updatehost {
// Picks up the last apply's result and any update already downloaded, and syncs the game's CheckInGame;
// `autoCheck` also looks at GitHub now, unless Settings › Updates turned automatic checks off.
void Start(bool autoCheck);
void Install();   // update.status, update.check, update.setAuto, update.apply and the "update" channel
// Writes the automatic-checks setting into the game folder's Melange.ini ([Update] CheckInGame) when it disagrees.
// Callers hold app::Tx() (or run before the server starts): setup apply, restore and choosing a folder call it.
void SyncInGameCheck();
// The new Melange.exe, started with --apply-update by the old one. Returns the process exit code.
int RunApply(const updater::ApplyArgs& args);
}  // namespace updatehost
}  // namespace melange::launcher
