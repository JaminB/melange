#pragma once
#include <string>

#include "store/compat.h"

namespace melange::launcher::storehost {
void Install();   // the store.* methods and channel, on the launcher's host; sweeps the chosen folder (below)
void Sync();      // follow the chosen game folder (main thread)
void Tick();      // the launcher's loop (main thread): runs a sweep that is due

// The compatibility sweep (store/compat.h) of the chosen game folder: local plugins that cannot load move to
// Mods\.incompatible\, Store ones are updated or removed by store::Reconcile (which fetches the list first, only when
// one of them needs it). Due once per folder after Sync opened it, and after RequestSweep; Tick runs it as soon as
// the folder may be written (the game is not running, no setup transaction, import or Store job). `melangeVersion`
// checks against that version instead of this Melange.exe's (an update just installed it); "" = MELANGE_VERSION.
// Any thread.
void RequestSweep(const std::string& melangeVersion = "");
// Runs it now on this thread. False, with nothing done, when it cannot run right now (see above); the Store half
// continues on the Store worker after this returns. `report` gets the local half's result.
bool SweepNow(const std::string& melangeVersion = "", compat::Report* report = nullptr);
}  // namespace melange::launcher::storehost
