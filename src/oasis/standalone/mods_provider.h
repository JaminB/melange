#pragma once
#include <string>
#include <vector>

#include "mods/spice.h"

// File-backed mods.list / mods.setEnabled for oasis.exe: spice::Parse of every <gameDir>\Mods\<id>\spice.json,
// resolved with spice::Resolve, against the plain "enabled" map of Mods\thumper-state.json. Deep Desert grants
// are read (a previously granted mod shows Enabled) but never written: granting still needs the in-game modal.
namespace melange::oasis::standalone::modsprov {
std::string ListJson(const std::wstring& gameDir, const std::string& melangeVersion);
// The manifests that resolve to Enabled (a Deep Desert grant counts), in load order.
std::vector<spice::Manifest> Enabled(const std::wstring& gameDir, const std::string& melangeVersion);
// 1 ok, 0 no such mod, -1 could not write thumper-state.json.
int SetEnabled(const std::wstring& gameDir, const std::string& id, bool on);
}  // namespace melange::oasis::standalone::modsprov
