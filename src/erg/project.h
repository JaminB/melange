#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The Erg project store: <dir>\<id>\project.ergpatch.json (the patch), meta.json and .lock. Saves are atomic, and a
// project a server has open is locked against every other server (the lock is an open handle, released on close or
// when the process ends). Within one server, the lock is a set of per-connection leases: Lock/Unlock add or drop the
// caller's lease, and the file closes only once the last one is gone. conn 0 is a caller with no connection of its
// own (a direct Store user, or an internal build with nothing to release it later).
namespace melange::erg::project {
constexpr const char* kPatchFile = "project.ergpatch.json";

struct Meta { std::string title, created, lastExport, lastTest; };
struct Info {
    std::string id, title, stem, base, modified;   // modified: ISO-8601 UTC of the patch file
    bool built = false;                             // exported or tested at least once
};
enum class LockResult { Ok, Busy, Missing, Failed };

bool ValidId(std::string_view id);                  // [a-z0-9]{1,24}
std::string NowIso();

class Store {
  public:
    explicit Store(std::wstring dir) : dir_(std::move(dir)) {}
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    const std::wstring& Dir() const { return dir_; }
    std::vector<Info> List() const;
    bool Exists(const std::string& id) const;
    // A free id from `hint` (itself, else hint2, hint3, ... within 24 characters).
    std::string FreeId(const std::string& hint) const;
    bool Create(const std::string& id, const std::string& patchJson, const Meta& meta, std::string* err);
    bool ReadPatch(const std::string& id, std::string* json, std::string* err) const;
    bool WritePatch(const std::string& id, const std::string& json, std::string* err);
    bool ReadMeta(const std::string& id, Meta* out) const;
    bool WriteMeta(const std::string& id, const Meta& meta, std::string* err);

    LockResult Lock(const std::string& id, uint64_t conn = 0);   // adds conn's lease; idempotent per (id, conn)
    void Unlock(const std::string& id, uint64_t conn = 0);       // drops conn's lease only
    void ReleaseConn(uint64_t conn);                             // drops every lease conn holds, across all projects
    bool Locked(const std::string& id) const;                    // true while any lease is held

  private:
    std::wstring Path(const std::string& id, const char* file) const;
    void UnlockLocked(const std::string& id, uint64_t conn);     // mx_ already held
    std::wstring dir_;
    mutable std::mutex mx_;
    std::map<std::string, void*> locks_;
    std::map<std::string, std::set<uint64_t>> leases_;
};
}  // namespace melange::erg::project
