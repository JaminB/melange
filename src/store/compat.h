#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "mods/spice.h"

// The compatibility sweep: a plugin this Melange can never load leaves Mods\ instead of sitting there as
// "incompatible". A plugin the user put there by hand moves to Mods\.incompatible\<folder>\; one installed from the
// Store is updated or removed by the Store engine (store::Reconcile). Every action leaves a notice the Mods pages
// show until it is dismissed. Shared by the game (Thumper, before its first scan) and Melange.exe (store_host.cpp).
// Disk only, no network: offline self-test.
namespace melange::compat {
// Why the plugin in `dir` can never load on `melangeVersion`, "" when it can: a spice.json that does not parse
// (including an unknown spiceVersion), a malformed melange.range, or one the version does not satisfy. A folder
// without spice.json (the implicit manifest) is always compatible. Not reasons: dependencies, conflicts, consent.
std::string Check(const std::wstring& dir, const std::string& melangeVersion, spice::Manifest* out = nullptr);

// The ids with a record in Mods\.store\installed.json: those count as Store plugins, whether or not the record's
// version still matches the folder.
std::set<std::string> StoreIds(const std::wstring& modsDir);
// A Store plugin for the Mods pages: a record, or a map pack an installed Store importer generated (generated.by).
bool IsStore(const std::set<std::string>& storeIds, const std::string& id, const std::string& generatedBy);

// Mods\.incompatible\notices.json, newest last, at most kMaxNotices.
constexpr size_t kMaxNotices = 50;
struct Notice {
    std::string key;       // unique within the file
    std::string id, name, version;
    std::string action;    // "quarantined" | "removed" | "updated" | "failed"
    std::string reason;    // why it could not load ("needs Melange >=0.4.0, you have 0.3.6")
    std::string melange;   // the Melange version it was checked against
    std::string at;        // UTC, ISO 8601
    std::string folder;    // quarantined: its folder under Mods\ (".incompatible\\<name>"), else ""
    std::string detail;    // updated: the new version; failed: what went wrong
};
std::wstring QuarantineDir(const std::wstring& modsDir);   // Mods\.incompatible
std::vector<Notice> LoadNotices(const std::wstring& modsDir);
// Fills key and at when empty. Returns the key, "" when the file could not be written.
std::string AddNotice(const std::wstring& modsDir, Notice n);
bool DismissNotice(const std::wstring& modsDir, const std::string& key);   // "" dismisses every notice
std::string NoticeJson(const Notice& n);
std::string NoticesJson(const std::vector<Notice>& all);
std::string Text(const Notice& n);   // one line for the Mods pages and the log

// A Store plugin that cannot load: store::Reconcile updates it to a compatible version or removes it.
struct Finding {
    std::string id, name, version, reason;
};
struct SweepContext {
    std::wstring modsDir;
    std::string melangeVersion;   // the running Melange, or the one an update just installed
    // Drops the plugin's enabled entry, Deep Desert grant and pins from thumper-state.json (the game edits its
    // in-memory state, Melange.exe the file). Called after the folder moved.
    std::function<void(const std::string& id)> forget;
};
struct Report {
    std::vector<Notice> quarantined;    // local plugins moved to Mods\.incompatible
    std::vector<Finding> store;         // Store plugins that cannot load: hand them to store::Reconcile
    std::vector<std::string> errors;    // "<folder>: why" for a move that failed (it stays, and does not load)
};
// Checks every Mods\<folder> (folders starting with '.' are never plugins) and quarantines the local ones that
// cannot load: Mods\<folder> moves to Mods\.incompatible\<folder> (or <folder>-2, -3, ...; never over an existing
// one) with a .melange-quarantine.json record inside, and a notice is added. Never touches a Store plugin.
// The caller makes sure nothing has a file open in Mods\ (the game: before Thumper's first scan; Melange.exe: the
// game is not running).
Report Sweep(const SweepContext& c);
}  // namespace melange::compat
