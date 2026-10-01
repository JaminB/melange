#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The plugin store's index.json, URL rules and version selection: pure functions (offline self-test).
namespace melange::store {
constexpr size_t kMaxIndexBytes = 1 << 20;
constexpr size_t kMaxPlugins = 500, kMaxVersions = 50, kMaxShots = 6;
constexpr uint64_t kMaxZipBytes = 256ull << 20, kMaxUnpackedBytes = 256ull << 20, kMaxShotBytes = 1 << 20;
constexpr int kMaxFiles = 2000;

struct Dep { std::string id, range; };
struct Shot { std::string path, sha256, caption; uint64_t size = 0; };
struct Version {
    std::string version, released, melange, kind, filesystem = "none", url, sha256, changelog;
    bool unsafe = false, yanked = false;
    std::vector<Dep> dependencies, conflicts;
    uint64_t size = 0, unpackedSize = 0;
    int files = 0;
};
struct Plugin {
    std::string id, name, description, homepage, licence;
    std::vector<std::string> authors, categories, gameBuilds;
    std::vector<Shot> screenshots;
    std::vector<Version> versions;   // newest first
};
struct Index {
    long long serial = 0;
    std::vector<Plugin> plugins;     // sorted by id
    std::vector<std::string> skipped;  // "<id>: why" for entries that failed validation
};

// Refuses the whole list for a bad shape or a cap; drops single plugins or versions that fail their checks.
bool ParseIndex(std::string_view text, Index* out, std::string* err);
const Plugin* FindPlugin(const Index& idx, std::string_view id);
const Version* FindVersion(const Plugin& p, std::string_view version);

enum class Scheme { Other, Https, File };
Scheme SchemeOf(std::string_view url);
bool CheckIndexUrl(std::string_view url, std::string* why);
// `ref` absolute (https://, or file:// under a file:// index) or relative to the index's folder.
bool ResolveUrl(std::string_view indexUrl, std::string_view ref, std::string* out, std::string* why);
bool FileUrlToPath(std::string_view url, std::wstring* path);   // file:///C:/a%20b/c -> C:\a b\c

struct Env {
    std::string melange;     // running Melange version
    std::string gameBuild;   // "1077", "" when the exe is not recognised
    bool rollback = false;   // the list is older than one seen before
};
struct Have {
    bool present = false, managed = false, pending = false;
    std::string version;
};
enum class Action { None, Install, Update, Remove };
const char* ActionName(Action a);
struct Choice {
    const Version* compatible = nullptr;
    Action action = Action::None;
    std::string state;   // "", "installed", "update", "manual", "yanked", "incompatible", "pending"
    std::string reason;
    bool canRemove = false;
};
bool Compatible(const Version& v, const Plugin& p, const Env& env, std::string* why);
Choice Choose(const Plugin& p, const Have& have, const Env& env);
extern const char* const kRollbackText;

struct Step { std::string id, version; };
// Dependencies first, then `id` itself; `installed` maps ids present in Mods\ to their versions.
bool PlanInstall(const Index& idx, const std::string& id, const std::string& version,
                 const std::map<std::string, std::string>& installed, const Env& env, std::vector<Step>* steps,
                 std::string* err);

bool Matches(const Plugin& p, std::string_view query);   // case-insensitive substring of name, id, authors, description
}  // namespace melange::store
