#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Read-only taps on the engine's own turn-end validation: GameStateValidationMsg Initialise 0x68a027 (build and
// send), ValidateNow 0x68a335 (check a received one; its "Reason N" failures) and the Net.Error funnel 0x7092cb
// that aborts the match with Net.OutOfSynch.
namespace melange::wormsign::enginecheck {
enum class Kind : uint8_t { Build, Check, Abort };
struct Record {
    Kind kind;
    uint32_t serial, tick, timeMs;
    uint8_t sov;          // source of validation: 1 time sync, 2 turn end
    bool result;          // Check: passed; Build: true; Abort: false
    uint16_t reasons;     // Check: bit n-1 set for each failed "Reason n"
    char error[40];       // Abort: the Net.Error key
};
bool Install();                                // creates the hooks disabled
bool SetActive(bool on);                       // on for online matches only
void Uninstall();
std::vector<Record> Records(uint32_t serial);  // this match's records, oldest first; main thread
bool FirstFailure(uint32_t serial, Record* out);
const char* ReasonText(int reason);            // 1..13
int FirstReason(uint16_t reasons);             // 0 when none; only where one code is needed
std::string ReasonNumbers(uint16_t reasons);   // every failed reason: "7,8,9,11,13"; "" when none
std::string ReasonTexts(uint16_t reasons);     // their texts in order, joined by "; "
// "reason 5: Random's dont match", "reasons 7,8: camera's active view matrix differs; camera's logical position
// differs"; "" when none.
std::string DescribeReasons(uint16_t reasons);
// "reasons" is the array of every failed reason number, "reason" their texts joined by "; ".
std::string Json(const std::vector<Record>& v);
}  // namespace melange::wormsign::enginecheck
