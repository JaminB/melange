#pragma once
#include <string>
#include <vector>

#include "launcher/setup/exe_check.h"

// Where is the game? Saved folder, Melange.exe's own folder, Steam libraries, GOG, uninstall entries.
namespace melange::launcher::setup {
enum class Hive { CurrentUser, LocalMachine };
// Registry reads go through here so the self-test can feed fake keys. `view32` picks WOW6432Node on 64-bit Windows.
class RegReader {
  public:
    virtual ~RegReader() = default;
    virtual bool String(Hive hive, const std::wstring& key, const std::wstring& value, std::wstring* out) = 0;
    virtual std::vector<std::wstring> Subkeys(Hive hive, const std::wstring& key) = 0;
};
RegReader& SystemRegistry();

struct Candidate {
    std::wstring path;
    std::string source;     // saved | self | steam | gog | manual
    std::wstring library;   // steam
    GameCheck check;
};
struct DetectInput {
    std::wstring saved, selfDir;
    RegReader* reg = nullptr;
    const std::vector<Profile>* profiles = nullptr;
};
std::vector<Candidate> Detect(const DetectInput& in);

// Steam library roots ("<root>" itself included), from the registry and libraryfolders.vdf.
std::vector<std::wstring> SteamLibraries(RegReader& reg);
// "steam" when `dir` is under a Steam library's steamapps\common, "gog" when GOG's keys list it, else "unknown".
std::string StoreOf(const std::wstring& dir, RegReader& reg);
// A Steam "common" folder (or any parent) picked by mistake: the child folder with WormsMayhem.exe, or "".
std::wstring ChildWithGame(const std::wstring& dir);
std::string CandidateJson(const Candidate& c);
}  // namespace melange::launcher::setup
