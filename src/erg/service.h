#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "erg/build.h"
#include "erg/install.h"
#include "erg/load.h"
#include "erg/patch.h"

// The Erg level service behind the level.* RPC methods: the same code in the game's Oasis and in oasis.exe. Every
// method is safe to call from a server thread (one call at a time inside); none touches the running game.
namespace melange::erg::service {
enum Code : int { kBadParams = -32602, kPolicy = -32000, kNotRunning = -32001, kBusy = -32002, kReadOnly = -32003 };

struct Env {
    std::wstring gameDir;                                 // the install; base files come from <game>\Data only
    std::wstring projectsDir;                             // Documents\Melange\erg\projects unless [Erg] ProjectsDir
    std::function<std::vector<install::Pack>()> packs;    // the map packs enabled now
    std::function<bool()> modsReadOnly;                   // oasis.exe while the game runs: no writes under Mods
    std::function<bool(const std::string& modId)> modActive;   // enabled this launch (else a restart is needed)
    std::function<bool(const std::string& fileName)> crcCollides;   // a built file would shadow a CRC-listed one
};

struct Blob {
    uint32_t ref = 0;
    std::string meta;                                     // {"ref","kind","frame"}
    std::string bytes;
};
struct Reply {
    bool ok = true;
    int code = 0;
    std::string message;
    std::string json = "null";
    std::vector<Blob> blobs;                              // level.load: one per scene blob, in blobs[] order
};

// A base level's files: a multiplayer entry of the install's registry (source "game", files under <game>\Data) or a
// level of a map pack (source "pack", files under the pack's assets\levels). An empty source tries both, game first.
struct BaseSource {
    load::BaseFiles files;
    std::wstring packRoot;                                // "" for a game base
};
bool ReadBase(const std::wstring& gameDir, const std::vector<install::RegistryEntry>& registry,
              const std::map<std::string, std::string>& strings, const std::vector<install::Pack>& packs,
              const std::string& key, const std::string& source, BaseSource* out, int* code, std::string* err);
// The level files of a patch against the install: the base is read and its sha256 checked, the patch applied and
// built. `stem` (when not empty) replaces the patch's stem.
bool BuildPatch(const std::wstring& gameDir, const std::vector<install::Pack>& packs, const Patch& p, const std::string& stem,
                std::vector<build::File>* out, int* code, std::string* err);

// The methods this service answers (level.test belongs to the Test component).
const std::vector<std::string>& Methods();
bool Mutating(std::string_view method);

class Service {
  public:
    explicit Service(Env env);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Reply Call(std::string_view method, std::string_view paramsJson);
    // level.test's build: the project's level files as ergtest_<id> into `root` (the Test workspace), with an empty
    // chunk when none is needed; stale outputs of an earlier build are removed. Result: {stem, title, files}.
    Reply BuildTest(const std::string& project, const std::wstring& root);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace melange::erg::service
