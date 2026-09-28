#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "melange/mods.h"

// Pure, offline-testable pieces of the content-identity and lobby-handshake logic (E). No Steam, no filesystem,
// no globals: everything here is a function of its arguments, so two peers that agree on the arguments agree on
// the result. handshake.cpp gathers the arguments (mod list, file hashes, Steam state) and calls these.
namespace melange::handshake {

constexpr int kSimApiVersion = 1;
// Every vanilla name is registered by frame 4 and none are added later (m2-design.md S1.5, [V]); the registry
// only grows past this once Thumper freezes mod message names, so the overhang is exactly modMessages.
constexpr uint32_t kVanillaMessageCount = 1227;
constexpr size_t kModsValueMaxBytes = 2000;

struct ContentFile {
    std::string relPath;    // forward slashes, lower case
    std::string sha256Hex;
};
struct ContentMod {
    std::string id, version;
    std::vector<ContentFile> files;  // any order in; BuildContentId sorts by relPath before hashing
};

// Deterministic text: the same mod set (in load order), the same file hashes and the same message count give
// the same text on every peer, independent of directory-walk order.
std::string CanonicalText(const std::vector<ContentMod>& modsInLoadOrder, uint32_t modMessages);
std::string HashOfCanonicalText(const std::string& canonical);  // sha256 hex, "" only on a hashing failure

// Sorts each mod's files by relPath, then builds the ContentId this peer would publish for this mod set.
mods::ContentId BuildContentId(std::vector<ContentMod> modsInLoadOrder, uint32_t modMessages);

// First 16 hex chars of ContentId.hash, or "v" for the vanilla (empty-hash) case.
std::string Hash16(const mods::ContentId& c);

// The "mlg" lobby member-data value: "1;<melangeVersion>;<hash16-or-v>;<contentMods>".
std::string BuildMlgValue(const std::string& melangeVersion, const mods::ContentId& c);
// False if `value` is missing/malformed (a vanilla peer, or a future/incompatible protocol version).
bool ParseMlgValue(const std::string& value, std::string* version, std::string* hash16, uint32_t* contentMods);

// "id@version,..." in load order, truncated to `maxBytes` (never mid-entry) with a trailing "…" when it does not
// all fit.
std::string BuildModsValue(const std::vector<ContentMod>& modsInLoadOrder, size_t maxBytes = kModsValueMaxBytes);

// Vanilla: no "mlg" key at all. MelangeVanilla: "mlg" present with hash16 "v". Match/Mismatch: hash16 compared
// against ours (mismatched when we ourselves are vanilla and they are not, or vice versa).
mods::PeerStatus ClassifyPeer(bool hasMlg, const std::string& theirHash16, const std::string& ourHash16);

// The lobby owner's "mlg.sim" value. "" means: do not write the key at all (we are vanilla; §5's guarantee that
// a vanilla host leaves no extra lobby-visible trace beyond its own "mlg" member key).
std::string BuildMlgSim(const std::string& ourHash16, bool weAreVanilla, const std::vector<std::string>& memberHash16s);

// May a sim mod run this match? Offline/local: always. Online: only if the lobby's "mlg.sim" is our hash16.
bool GateAllowsSim(bool online, const std::string& ourHash16, const std::string& lobbySim);

}  // namespace melange::handshake
