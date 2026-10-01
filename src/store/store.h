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
    std::vector<VersionRow> versions;
    std::vector<std::string> dependants;         // installed, enabled mods that need this one
    std::vector<std::string> conflictsEnabled;   // installed, enabled mods it conflicts with
    std::vector<Step> plan;                      // what installing `compatible` would install, dependencies first
    std::string planError;
};
struct Outcome { int code = 0; std::string message; };   // 0 started, -32000 refused, -32002 busy, -32602 bad params

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
void InstallRpc();
void PublishState();                                   // any thread
std::string ChannelJson();                             // any thread
// store_page.cpp
void RegisterPage();
}  // namespace melange::store
