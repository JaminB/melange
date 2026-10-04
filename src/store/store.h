#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "store/index.h"

// The Store module's state and actions, shared by the overlay page, the Oasis methods and the Mods page. Main thread
// unless marked "any thread". Nothing here touches the network until Refresh() or an action asks for it.
namespace melange::store {
struct Job {
    std::string phase = "idle";   // idle fetching downloading verifying installing removing done error
    std::string id, version, message;
    uint64_t bytes = 0, total = 0;
};
struct Status {
    bool enabled = false;
    std::string indexUrl;
    bool customIndex = false;
    std::string fetchedAt;        // local "YYYY-MM-DD HH:MM", "" before the first fetch
    bool offline = false, fetching = false, haveIndex = false, rollback = false, busy = false;
    long long serial = -1;
    size_t plugins = 0;
    std::string error;            // the last fetch's failure
    Job job;
    std::string gate;             // "" or why installs, updates and removes are refused right now
    std::vector<std::string> pending, notices;
};
struct Item {
    std::string id, name, description, licence, kind, latest, compatible, state, reason, error;
    std::vector<std::string> authors, categories;
    bool unsafe = false;
    uint64_t size = 0;
    bool installed = false, managed = false, enabled = false;
    std::string installedVersion, installedState;
    Action action = Action::None;
    bool canRemove = false;
};
struct ListQuery {
    std::string query, category, filter = "all";   // filter: all | installed | updates
    bool incompatible = false;
};
struct VersionRow {
    std::string version, released, melange, changelog;
    uint64_t size = 0;
    bool yanked = false, compatible = false;
};
struct ShotRow { int n = 0; std::string caption; bool ready = false; };
struct Details {
    Item item;
    std::string homepage, filesystem;
    bool content = false;
    std::vector<Dep> dependencies, conflicts;
    std::vector<ShotRow> screenshots;
    std::vector<ImportLine> imports;
    int importedMaps = 0;                        // levels in the packs its importer made on this PC
    std::vector<VersionRow> versions;
    std::vector<std::string> dependants;         // installed, enabled mods that need this one
    std::vector<std::string> conflictsEnabled;   // installed, enabled mods it conflicts with
    std::vector<Step> plan;                      // what installing `compatible` would install, dependencies first
    std::string planError;
};
struct Outcome { int code = 0; std::string message; };   // 0 started, -32000 refused, -32002 busy, -32602 bad params

// The Store engine runs in the game (game_host.cpp) and in Melange.exe; the host provides what differs.
struct LocalMod {
    std::string id, version, state;
    bool enabled = false, sessionActive = false, contentRelevant = false;
    std::vector<std::string> dependencies, conflicts;
};
class Host {
  public:
    virtual ~Host() = default;
    virtual std::string MelangeVersion() = 0;
    virtual std::string GameBuild() = 0;                 // "1077", "" when the exe is not recognised
    virtual std::string Gate() = 0;                      // "" or why changes are refused now
    virtual std::vector<LocalMod> InstalledMods() = 0;   // every mod in Mods\, Thumper's view
    virtual void Placed(const std::string& id, bool enable) = 0;      // a new Mods\<id> is in place
    virtual void Unload(const std::string& id) = 0;                   // before Mods\<id> is replaced or removed
    virtual void Reload(const std::string& id, bool enable) = 0;      // after Mods\<id> was replaced
    virtual void Forget(const std::string& id) = 0;                   // after Mods\<id> was removed
    virtual void DeleteData(const std::string& id) = 0;               // its [Mod.<id>] settings and saved data
};
struct Config {
    std::string indexUrl;
    bool custom = false, showIncompatible = false;
    uint64_t maxDownload = 64ull << 20;
};
extern const char* const kDefaultIndex;
void SetHost(Host* h, const Config& c);
// Point the engine at a Mods folder: load installed.json and pending.json. With `droppedIds` (the game, before
// Thumper's first scan), deferred updates and removes run first and the removed ids are returned. False while a job
// runs.
bool Open(const std::wstring& modsDir, std::vector<std::string>* droppedIds = nullptr);
void Close();                                  // no Mods folder (Melange.exe before a game is chosen)
void Tick();                                   // the host's loop: gate changes, update markers
void MarkDirty();                              // installed mods changed
void Shutdown();
std::string IndexText();                       // the last good index.json text ("" before one); any thread

bool Active();                                 // any thread
Status GetStatus();
std::vector<Item> List(const ListQuery& q);
bool GetDetails(const std::string& id, Details* out, bool fetchShots);
Outcome Refresh();
void EnsureFetched();                          // the first view this session fetches once
Outcome Install(const std::string& id, const std::string& version, bool enable, bool replaceManual);
Outcome Update(const std::string& id);
Outcome Remove(const std::string& id, bool deleteData);
bool Cancel();
bool OpenHomepage(const std::string& id, std::string* err);
std::wstring ShotPath(const std::string& id, int n);   // any thread; "" until fetched and verified
bool UpdateAvailable(const std::string& id);           // the Mods page marker, once a list was fetched this session

// store_rpc.cpp
void InstallRpc(bool gameOnly = true);
void PublishState();                                   // any thread
std::string ChannelJson();                             // any thread
// store_page.cpp
void RegisterPage();
}  // namespace melange::store
