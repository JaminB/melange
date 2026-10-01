#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mods/spice.h"
#include "store/fetch.h"
#include "store/zipcheck.h"

// The Store's disk side, without the game: verify, inspect, stage, place, replace, remove, installed.json and
// pending.json under Mods\.store\. Offline self-test.
namespace melange::store::install {
struct Paths {
    std::wstring mods;   // Mods
    std::wstring root;   // Mods\.store
    std::wstring Dl() const { return root + L"\\dl"; }
    std::wstring Stage() const { return root + L"\\stage"; }
    std::wstring Old() const { return root + L"\\old"; }
    std::wstring Cache() const { return root + L"\\cache"; }
};
Paths MakePaths(const std::wstring& modsDir);
bool EnsureDirs(const Paths& p);

struct Expect {   // what the index promised
    std::string id, version, sha256, kind = "client-only", filesystem = "none";
    bool unsafe = false;
    uint64_t size = 0, unpackedSize = 0;
};
std::string EffectiveKind(const spice::Manifest& m);

bool VerifyFile(const std::wstring& path, uint64_t size, const std::string& sha256, std::string* err);
// Downloads to `part`, hashing as it streams; the length and SHA-256 must match, else `part` is deleted.
bool FetchVerified(const std::string& url, const std::wstring& part, const Expect& e, uint64_t cap,
                   const fetch::Options& base, std::string* err);
bool Inspect(const std::wstring& zip, const std::string& id, uint64_t maxTotal, std::vector<zipcheck::Entry>* out,
             std::string* err);

struct Staged {
    std::wstring root;   // Mods\.store\stage\<id>-<rand>
    std::wstring dir;    // root\<id>
    std::string rel;     // "stage\<id>-<rand>", for pending.json
};
// Inspect, extract into a fresh staging folder and check the result against `e`. Cleans up after itself on failure.
bool Stage(const Paths& p, const std::wstring& zip, const Expect& e, const std::string& melangeVersion, Staged* out,
           std::string* err, const std::atomic<bool>* cancel = nullptr);

// Renames go through here so tests can make one fail. Returns 0 or a Win32 error.
using MoveFn = std::function<unsigned long(const std::wstring& from, const std::wstring& to)>;
unsigned long DefaultMove(const std::wstring& from, const std::wstring& to);

enum class Result { Done, Pending, Failed };
// A new folder Mods\<id>.
Result PlaceNew(const Paths& p, const std::string& id, const Staged& s, std::string* err, const MoveFn& mv = DefaultMove);
// Mods\<id> exists: swap it for the staged folder, carrying Mods\<id>\user\ over; rolls back if the swap fails and
// returns Pending when a file in Mods\<id> is in use.
Result Replace(const Paths& p, const std::string& id, const Staged& s, std::string* err, const MoveFn& mv = DefaultMove);
Result Remove(const Paths& p, const std::string& id, std::string* err, const MoveFn& mv = DefaultMove);
bool DeleteTree(const std::wstring& dir);
bool Exists(const std::wstring& path);

struct Record { std::string version, sha256, installedAt; long long serial = 0; };
struct Db {
    std::map<std::string, Record> mods;
    long long serialSeen = -1;
};
bool LoadDb(const Paths& p, Db* db);
bool SaveDb(const Paths& p, const Db& db);

struct Pending {
    std::string op;      // "update" | "remove"
    std::string id, version, sha256, stage;
    long long serial = 0;
    bool deleteData = false;
};
bool LoadPending(const Paths& p, std::vector<Pending>* out);
bool SavePending(const Paths& p, const std::vector<Pending>& ops);

struct Applied { Pending op; bool ok = false; std::string message; };
// The next launch, before Thumper's first scan: each op is re-checked and applied; still-blocked ops stay queued.
std::vector<Applied> ApplyPending(const Paths& p, Db* db, const MoveFn& mv = DefaultMove);
// dl\, old\ and stage\ leftovers, except staging folders that pending.json still needs.
void CleanLeftovers(const Paths& p);

std::string NowIso();
bool WriteFileAtomic(const std::wstring& path, const std::string& data);
}  // namespace melange::store::install
