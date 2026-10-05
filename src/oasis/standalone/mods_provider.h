#pragma once
#include <string>
#include <vector>

#include "mods/spice.h"

// File-backed mods.list / mods.setEnabled for oasis.exe: spice::Parse of every <gameDir>\Mods\<id>\spice.json,
// resolved with spice::Resolve, against the plain "enabled" map of Mods\thumper-state.json. Deep Desert grants
// are read (a previously granted mod shows Enabled) but never written: granting still needs the in-game modal.
// A folder whose spice.json does not parse is listed as incompatible, as the game's Mods page shows it.
namespace melange::oasis::standalone::modsprov {
std::string ListJson(const std::wstring& gameDir, const std::string& melangeVersion);
// The manifests that resolve to Enabled (a Deep Desert grant counts), in load order.
std::vector<spice::Manifest> Enabled(const std::wstring& gameDir, const std::string& melangeVersion);
// 1 ok, 0 no such mod, -1 could not write thumper-state.json.
int SetEnabled(const std::wstring& gameDir, const std::string& id, bool on);
// A plugin left Mods\: its enabled entry, Deep Desert grant and pins go. False if thumper-state.json could not be
// written (true when there was nothing to drop).
bool Forget(const std::wstring& gameDir, const std::string& id);
// thumper-state.json "showLocal" (the Mods pages also list plugins not installed from the Store), shared with the game.
bool ShowLocal(const std::wstring& gameDir);
bool SetShowLocal(const std::wstring& gameDir, bool on);
// mods.view: {showLocal, notices} (store/compat.h notices).
std::string ViewJson(const std::wstring& gameDir);
}  // namespace melange::oasis::standalone::modsprov
