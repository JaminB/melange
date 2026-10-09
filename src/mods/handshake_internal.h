#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "melange/mods.h"

// Pure, offline-testable pieces of the content-identity and lobby-handshake logic (E). No Steam, no filesystem,
// no globals: everything here is a function of its arguments, so two peers that agree on the arguments agree on
// the result. handshake.cpp gathers the arguments (mod list, file hashes, Steam state) and calls these.
namespace melange::handshake {

constexpr int kSimApiVersion = 1;
constexpr size_t kModsValueMaxBytes = 2000;

struct ContentFile {
    std::string relPath;    // forward slashes, lower case
    std::string sha256Hex;
};
struct ContentMod {
    std::string id, version;
    std::vector<ContentFile> files;  // any order in; BuildContentId sorts by relPath before hashing
};
struct ModMessage {
    std::string name;
    uint16_t id = 0;
};
struct CloneSpec {
    uint16_t k = 0;
    int32_t vid = 0;
    std::string name, base, mod;
    int cell = -1;
    std::string bankSha256;                                  // "" when the clone has no bank
    std::vector<std::pair<std::string, std::string>> set;    // field, canonical value (SetNumber/SetBool/SetString)
};

std::string SetNumber(double v);
std::string SetBool(bool v);
std::string SetString(const std::string& v);   // percent-encodes anything outside [A-Za-z0-9._/-]

// "clone <k> <vid> <name> <base> <cell> <bank sha256 or -> <set as sorted key=value, space separated, or ->".
std::string CloneLine(const CloneSpec& c);

// Deterministic text, version 2: the mod set (in load order) with its file hashes, the mod messages as name=id in
// registration order, one line per declared clone in k order, and (only with clones) the local [Weapons]
// ExtraPerExplosion: a per-machine setting, but one that changes how many extra explosions a clone can queue, so
// peers whose clones went live must agree on it too. Defaults to 8, the ini default, for callers that don't care.
std::string CanonicalText(const std::vector<ContentMod>& modsInLoadOrder, const std::vector<ModMessage>& messages,
                          const std::vector<CloneSpec>& clones, int extraPerExplosion = 8);
std::string HashOfCanonicalText(const std::string& canonical);  // sha256 hex, "" only on a hashing failure

// Sorts each mod's files by relPath, then builds the ContentId this peer would publish for this content.
mods::ContentId BuildContentId(std::vector<ContentMod> modsInLoadOrder, const std::vector<ModMessage>& messages,
                               const std::vector<CloneSpec>& clones, int extraPerExplosion = 8);

// First 16 hex chars of ContentId.hash, or "v" for the vanilla (empty-hash) case.
std::string Hash16(const mods::ContentId& c);

// The "mlg" lobby member-data value: "1;<melangeVersion>;<hash16-or-v>;<contentMods>".
std::string BuildMlgValue(const std::string& melangeVersion, const mods::ContentId& c);
// False if `value` is missing/malformed (a vanilla peer, or a future/incompatible protocol version).
bool ParseMlgValue(const std::string& value, std::string* version, std::string* hash16, uint32_t* contentMods);

// "id@version,..." in load order, truncated to `maxBytes` (never mid-entry) with a trailing "…" when it does not
// all fit.
std::string BuildModsValue(const std::vector<ContentMod>& modsInLoadOrder, size_t maxBytes = kModsValueMaxBytes);

// What differs between two "mlg.mods" values, for the lobby panel: "missing a@1, extra b@2, c 1.0.0 vs 1.1.0";
// "" when the lists name the same mods at the same versions.
std::string DiffModsValues(const std::string& ours, const std::string& theirs);

// Vanilla: no "mlg" key at all. MelangeVanilla: "mlg" present with hash16 "v". Match/Mismatch: hash16 compared
// against ours (mismatched when we ourselves are vanilla and they are not, or vice versa).
mods::PeerStatus ClassifyPeer(bool hasMlg, const std::string& theirHash16, const std::string& ourHash16);

// The lobby owner's "mlg.sim" value. "" means: do not write the key at all (we are vanilla: a vanilla host must
// leave no extra lobby-visible trace beyond its own "mlg" member key).
std::string BuildMlgSim(const std::string& ourHash16, bool weAreVanilla, const std::vector<std::string>& memberHash16s);

// May a sim mod run this match? Offline/local: always. Online: only if the lobby's "mlg.sim" is our hash16.
bool GateAllowsSim(bool online, const std::string& ourHash16, const std::string& lobbySim);

// Clone keys. "mlg.wpn" (member) = "1;<hash16 of the clone lines and ExtraPerExplosion>;<count>", "" when there are
// no clones. extraPerExplosion defaults to 8, the ini default, for callers that don't care.
std::string CloneHash16(const std::vector<CloneSpec>& clones, int extraPerExplosion = 8);
std::string BuildWpnValue(const std::vector<CloneSpec>& clones, int extraPerExplosion = 8);
bool ParseWpnValue(const std::string& value, std::string* hash16, uint32_t* clones);
// "mlg.req" (lobby, owner only) = "wpn1;<hash16 of the owner's content>", "" (removed) without clones.
std::string BuildReqValue(const std::string& ourHash16, bool haveClones);
bool ParseReqValue(const std::string& value, std::string* hash16);

// "mlg.msg" (member) = "name=id,..." in registration order, truncated like mlg.mods; "" without mod messages.
std::string BuildMsgValue(const std::vector<ModMessage>& messages, size_t maxBytes = kModsValueMaxBytes);
// "message ids differ: A.B 1104 vs 1105, missing C.D, extra E.F", or "" when both name the same pairs in the
// same order. An order-only difference is reported as "message order differs: ...".
std::string DiffMsgValues(const std::string& ours, const std::string& theirs);

// The messages whose live registry id no longer equals the hashed one (lookup returns 0xffff when unregistered).
std::vector<std::string> ChangedMessageIds(const std::vector<ModMessage>& hashed, uint16_t (*lookup)(const char*));

// The clone lobby policy of the weapon handshake.
struct LobbyMember {
    std::string name;
    bool hasMlg = false;
    std::string hash16;     // from "mlg"
    std::string diff;       // what differs from us (mods, then messages), "" if unknown
};
struct CloneLobbyInput {
    bool inLobby = false, weAreOwner = false, haveClones = false;
    std::string ourHash16;
    std::string lobbyReq;           // the lobby's "mlg.req" ("" when absent)
    std::string hostMods;           // the owner's "mlg.mods"
    std::string diffToHost;         // what differs between us and the owner
    std::vector<LobbyMember> members;   // everyone but us
};
struct CloneVerdict {
    bool ok = true;                 // clones may be live with these members
    bool hostHeld = false;          // we host with clones and a member does not match: refuse or suspend
    bool joinerMismatch = false;    // we joined a clone lobby our content does not match: the modal
    std::string why;
    std::vector<std::string> members;   // "<name>: <reason>" per offending member (host side)
};
CloneVerdict EvaluateCloneLobby(const CloneLobbyInput& in);

// Lower case with backslashes turned into forward slashes (path comparison keys).
std::string LowerSlashes(std::string s);

// Game-file integrity ("mlg.gid", member): which exe and which retail data files this peer runs, so the lobby can
// warn when two Melange peers would desync over a third-party mod (MMP, Renewation, a Data2 overlay). Warning
// only: never part of the content hash, never gates anything.
constexpr uint32_t kGidData2 = 1, kGidCrcOff = 2;   // a Data2 folder exists; the exe's CRC check is bypassed
struct GameId {
    std::string exe16, data16;   // first 16 hex of the exe file's sha256, and of GidDataText's sha256
    uint32_t flags = 0;
};
// The paths that go into data16: the retail CRC table and every file of a Data2 overlay ("data2/..."), except
// Data/Language/ and Data2/Language/ (text only, never sim).
bool GidHashesPath(const std::string& path);
// "melange-gid/1\n" then "file=<path lower case, forward slashes> <sha256 or ->\n" sorted by path; files is
// (path, sha256) with "" for a missing file. Paths GidHashesPath refuses are skipped.
std::string GidDataText(const std::vector<std::pair<std::string, std::string>>& files);
// "1;<exe16>;<data16>;<flags>", "" when either hash is not 16 hex chars (a hashing failure: publish nothing).
std::string BuildGidValue(const GameId& g);
bool ParseGidValue(const std::string& value, GameId* out);
// "Data2 overlay, CRC check off", "" for no flags.
std::string GidFlagsText(uint32_t flags);
// What differs between our fingerprint and a peer's, "" when nothing does, joined by "; ": "different game build",
// "different game data files" (data16 or the Data2 flag; followed by "(theirs: Data2 overlay; yours: ...)" for
// whoever has one), and "their/your game's CRC check is off" when only one side bypasses it (an exe or loader
// property, not a data one).
std::string DiffGid(const GameId& ours, const GameId& theirs);
// One "<name>: <DiffGid>" line per member whose "mlg.gid" parses and differs from ours. Members without the key
// (vanilla or an older Melange) are unknown and skipped; so is everyone when our own value does not parse.
struct GidMember {
    std::string name, gid;
};
std::vector<std::string> GidWarnings(const std::string& ourGid, const std::vector<GidMember>& members);

}  // namespace melange::handshake
