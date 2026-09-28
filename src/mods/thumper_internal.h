#pragma once
// Plumbing shared between Thumper's own files (thumper.cpp, spice.cpp is pure and doesn't need this,
// thumper_state.cpp, consent.cpp, mods_page.cpp) and render/mirage/modfs.cpp, which A also reimplements.
// Not a frozen M2 contract: only code inside component A depends on it.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "mods/spice.h"

namespace melange::thumper {

// One discovered mod, after the latest scan + resolve pass.
struct Entry {
    spice::Manifest manifest;
    std::wstring dir;
    mods::State state = mods::State::Disabled;  // as shown on the Mods page / mods::List()
    std::string reason;
    std::string authorsJoined;     // stable storage for ModInfo::authors (comma-joined)
    int order = -1;
    bool contentRelevant = false;  // content kind, entry.sim, messages, or unsafe (decision 4)
    bool sessionActive = false;    // loading this session (frozen at launch for contentRelevant mods)
    bool deepDesertGranted = false;
};

// A thread-safe, read-only copy of the current entries, in mods::List() order.
std::vector<Entry> Snapshot();
bool FindEntry(const std::string& id, Entry* out);

// render/mirage/modfs.cpp's data source: every mod active this session, lowest priority first.
struct SessionRoot {
    std::string id;
    std::wstring dir;
};
std::vector<SessionRoot> ActiveRoots();

// Re-scans <ModsDir>\* and re-resolves. Safe to call repeatedly (a manifest or mod-set change, a user
// toggle, or the file watcher). Fires mods::OnChange on the main thread if anything visible changed.
void Rescan();

// User actions (main thread). Both persist to thumper-state.json and re-resolve.
bool SetEnabled(const std::string& id, bool on);
bool SetDeepDesert(const std::string& id, bool granted);

// Deep Desert consent (consent.cpp).
std::string GrantHash(const spice::Manifest& m);  // sha256 of permissions + entry.client's bytes
bool IsGranted(const Entry& e);                    // granted AND the stored hash still matches
void DrawConsentModals();                          // called once per frame by mods_page.cpp
void RequestConsent(const std::string& id);         // opens the modal for `id` (Deep Desert, on enable)
void DrawDeepDesertMarker();                        // the persistent HUD corner marker

// The overlay panels (mods_page.cpp).
void RegisterPanels();

// thumper_state.cpp
struct PinEntry { std::string id, before, after; };  // exactly one of before/after is non-empty
struct DeepDesertRecord { bool granted = false; std::string grantHash, author, at; };
struct State {
    int version = 1;
    std::map<std::string, bool> enabled;
    std::vector<PinEntry> pins;
    std::map<std::string, DeepDesertRecord> deepDesert;
    bool migratedDisabledMods = false;
};
State& Live();               // the in-memory state, mutated in place; call Save() after changing it
bool Load();                  // reads Mods\thumper-state.json (or the Documents fallback); missing = defaults
bool Save();                  // atomic temp+rename; picks the same path Load() used or resolved at first Save()
std::wstring StatePath();     // resolved once: <ModsDir>\thumper-state.json, or the Documents fallback
}  // namespace melange::thumper
