#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "melange/wormsign.h"

// The desync bundle: a zip with report.json, both sides' detail records, diff.txt, the match recording, the last
// two minutes of logs, mods.json and sysinfo.txt. Building it is pure; writing it is the caller's.
namespace melange::wormsign::bundle {
struct Contributor {
    std::string name;
    uint32_t version = 1;
    uint64_t ours = 0, theirs = 0;
    bool haveOurs = false, haveTheirs = false;
};
struct Inputs {
    Divergence div{};
    uint64_t ours[kEngineComps] = {}, theirs[kEngineComps] = {};
    bool haveOurs = false, haveTheirs = false;
    bool contribListsMatch = true;
    std::vector<Contributor> contributors;
    // Our logic RNG at the end of recent ticks (tick, value), ending at the divergence tick, newest last. With both
    // detail records this lets diff.txt say how many draws apart the two machines are and which of our ticks the
    // peer's value matches.
    std::vector<std::pair<uint32_t, uint32_t>> rngTrail;
    std::string peerName;                    // as the lobby shows it; hashed in the files
    std::string peersJson = "[]";            // array; SteamIDs already hashed with `salt`
    std::string engineJson = "[]";           // array of engine validation records
    std::string correlation;                 // one line, "" when the engine has not checked since
    std::string detailLocal, detailPeer;     // JSON, "" when unknown
    std::string melangeLog, jlog;            // already cut to the last two minutes
    std::string modsJson = "[]", sysinfo;
    std::string melangeVersion, exeBuild;
    std::vector<uint8_t> recording;          // the match .wsr, empty when there is none
    std::string recordingName = "match.wsr";
    // Redaction, as "Save logs as": SteamIDs and IPs hashed with `salt`, user and computer names replaced.
    std::string salt, userName, profileName, computerName;
};

std::string PeerRef(const std::string& salt, uint64_t steamId);   // "hash:xxxxxxxx", "" for 0
// The game's logic RNG is the LCG x' = x * 1103515245 + 12345 (mod 2^32). Steps from `from` to `to`, -1 when `to`
// is not within `max` draws.
int RngSteps(uint32_t from, uint32_t to, int max = 1 << 16);
// The "# rng ..." line of diff.txt for two detail records (their "rng" fields), "" when it cannot be said.
std::string RngLine(const Inputs& in);
std::string ReportJson(const Inputs& in);
std::string DiffText(const Inputs& in);
std::string Redact(const Inputs& in, std::string text);
bool BuildZip(const Inputs& in, std::string* zip, std::vector<std::string>* entries = nullptr);
// desync-<yyyymmdd-hhmmss>-p<pid>-m<serial>-t<tick>.zip (the pid keeps two instances on one machine apart)
std::wstring FileName(uint32_t serial, uint32_t tick, int y, int mo, int d, int h, int mi, int s, uint32_t pid);
}  // namespace melange::wormsign::bundle
