#pragma once
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// The Erg project store: <dir>\<id>\project.ergpatch.json (the patch), script.lua, meta.json and .lock. Saves are
// atomic, and a project a server has open is locked against every other server (the lock is an open handle, released
// on close or when the process ends).
namespace melange::erg::project {
constexpr const char* kPatchFile = "project.ergpatch.json";
constexpr const char* kScriptFile = "script.lua";

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
    // The level script, "" when the project has none; writing "" removes it.
    bool ReadScript(const std::string& id, std::string* text, std::string* err) const;
    bool WriteScript(const std::string& id, const std::string& text, std::string* err);
    bool ReadMeta(const std::string& id, Meta* out) const;
    bool WriteMeta(const std::string& id, const Meta& meta, std::string* err);

    LockResult Lock(const std::string& id);         // idempotent for this store
    void Unlock(const std::string& id);
    bool Locked(const std::string& id) const;

  private:
    std::wstring Path(const std::string& id, const char* file) const;
    std::wstring dir_;
    mutable std::mutex mx_;
    std::map<std::string, void*> locks_;
};
}  // namespace melange::erg::project
