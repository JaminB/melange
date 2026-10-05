#pragma once
// Plumbing shared between Thumper's own files (thumper.cpp, spice.cpp is pure and doesn't need this,
// thumper_state.cpp, consent.cpp, mods_page.cpp) and render/mirage/modfs.cpp, which A also reimplements.
// Thumper-internal: not a public contract.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mods/spice.h"
#include "store/compat.h"

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

// Map packs changed at the menu (levels/live.cpp). The candidates are installed mods with a levels array whose
// launch state and preference may differ; SetLive records the session-only state (the content set stays the launch
// one) and persists the preference like SetEnabled.
std::vector<Entry> LiveCandidates();
void SetLive(const std::string& id, bool on);
bool LiveState(const std::string& id, bool* on);   // any thread
bool SetDeepDesert(const std::string& id, bool granted);

// A random value generated once per install and kept in Melange.ini, outside Mods\: mixed into GrantHash so a
// mod archive cannot ship a pre-computed grant record for itself (thumper_state.cpp lives under Mods\, which a
// mod's own files can reach; Melange.ini does not).
const std::string& GrantSalt();

// Deep Desert consent (consent.cpp).
std::string GrantHash(const spice::Manifest& m);  // sha256 of the salt + permissions + entry.client's bytes + authors
bool IsGranted(const Entry& e);                    // granted AND the stored hash still matches
void DrawConsentModals();                          // called once per frame by mods_page.cpp
void RequestConsent(const std::string& id);         // opens the modal for `id` (Deep Desert, on enable)
void DrawDeepDesertMarker();                        // the persistent HUD corner marker

// The overlay panels (mods_page.cpp).
void RegisterPanels();

// What the Mods pages show beside the list (store/compat.h): which plugins came from the Store, and the compatibility
// sweep's notices. Read from Mods\.store\installed.json and Mods\.incompatible\notices.json, cached for 2 s
// (`refresh` re-reads now). Any thread.
struct View {
    std::set<std::string> storeIds;
    std::vector<compat::Notice> notices;
};
View CurrentView(bool refresh = false);
bool IsStore(const Entry& e, const View& v);
void SetShowLocal(bool on);                       // main thread; persists to thumper-state.json
bool DismissNotice(const std::string& key);       // "" dismisses every notice

// thumper_state.cpp
struct PinEntry { std::string id, before, after; };  // exactly one of before/after is non-empty
struct DeepDesertRecord { bool granted = false; std::string grantHash, author, at; };
struct State {
    int version = 1;
    std::map<std::string, bool> enabled;
    std::vector<PinEntry> pins;
    std::map<std::string, DeepDesertRecord> deepDesert;
    bool migratedDisabledMods = false;
    bool showLocal = false;   // the Mods pages also list plugins not installed from the Store (display only)
};
State& Live();               // the in-memory state, mutated in place; call Save() after changing it
bool Load();                  // reads Mods\thumper-state.json (or the Documents fallback); missing = defaults
bool Save();                  // atomic temp+rename; picks the same path Load() used or resolved at first Save()
std::wstring StatePath();     // resolved once: <ModsDir>\thumper-state.json, or the Documents fallback
}  // namespace melange::thumper
