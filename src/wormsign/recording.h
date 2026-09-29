#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "melange/wormsign.h"
#include "wormsign/format.h"
#include "wormsign/records.h"

// A .wsr recording loaded for replay: header facts, seeds, pre-match draws, inputs, tick hashes and the setup.
namespace melange::wormsign {
struct Recording {
    int format = 0, engineHash = 0;
    bool complete = false, online = false;
    std::string exeBuild, contentHash, melange, contributors;  // contributors: "name@version,..." in name order
    std::vector<rec::Seed> seeds;
    std::vector<rec::Draw> draws;
    std::vector<rec::Input> inputs;                           // in call order (callT ascending)
    uint32_t firstTick = 0, lastTick = 0;                     // 0, 0 when no ticks
    std::vector<TickHash> ticks;                              // index tick - firstTick
    std::vector<uint8_t> have;                                // per index: 0 for a gap
    std::string setup;                                        // SETP JSON, "" if none
    std::vector<std::string> contribNames;                    // HEAD contributors, in name order
    std::vector<rec::ContribChange> contribChanges;           // CTRB, in tick order

    bool Tick(uint32_t tick, TickHash* out) const;
    uint32_t TickCount() const;
    // Per-contributor hashes at `tick` (contribNames order) from the CTRB changes; false when none were recorded.
    bool ContribHashes(uint32_t tick, std::vector<uint64_t>* out) const;
};

// Decodes every chunk the player needs. False with *err for a file that cannot be replayed at all.
bool LoadRecording(const wsr::Reader& r, Recording* out, std::string* err);

struct ArmEnv {
    std::string exeBuild = "1077";
    std::string contentHash;          // ours, "" when vanilla
    bool anyContent = false;          // [Wormsign] ReplayAnyContent
    bool inLobby = false, online = false, inMatch = false;
};
// Why this recording cannot be armed here, or "" when it can.
std::string ArmRefusal(const Recording& rec, const ArmEnv& env);

// "name@version,..." from a HEAD "contributors" array of {name, version} objects or "name@version" strings.
std::string ContributorKey(const std::string& headJson);
}  // namespace melange::wormsign
