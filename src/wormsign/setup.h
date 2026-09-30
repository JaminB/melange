#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The match setup fingerprint (the SETP chunk): level, land, theme, data bank, scheme and teams, as a JSON object.
// The recorder stores it after tick 1; the player takes it again at tick 1 of a replay and compares.
namespace melange::wormsign::setup {
constexpr int kVersion = 1;

struct Team {
    std::string name;
    uint32_t worms = 0;
};
struct Data {
    std::string level, landFile, landTheme, dataBank, timeOfDay, levelDetails, lastScheme, schemeName;
    std::string levelSim;            // sha256 of the level script text the match loaded; "" for none
    uint64_t scheme = 0, init = 0;   // FNV of GM.SchemeData / GM.GameInitData (strings dereferenced, refs skipped)
    std::string schemeRaw;           // hex of the scheme's integer settings, for diagnosing a mismatch
    bool haveScheme = false, haveInit = false;
    std::vector<Team> teams;
};

std::string ToJson(const Data& d);
// True when `live` agrees with every field `recorded` has; otherwise *why names the first difference.
// A recording with a newer fingerprint version is not compared (true, *why says so).
bool Compare(std::string_view recorded, std::string_view live, std::string* why);

// Reads the live setup. Main thread, in a match (after the level has loaded).
bool Capture(Data* out);
}  // namespace melange::wormsign::setup
