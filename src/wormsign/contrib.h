#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Hash contributors behind melange/wormsign.h AddContributor.
namespace melange::wormsign::contrib {
uint64_t HashTick(uint32_t tick);              // FNV over the contributors due this tick; 0 when none
// The contributor set as "name@version,..." in name order, "" when none. A replay compares the recorded mods hashes
// only when the recording's HEAD lists the same set; while any contributor has inReplayCompare=false this returns a
// key no recording has ("!" prefix), so the mods hashes are not compared.
std::string ReplayKey();

constexpr uint32_t kRingTicks = 512;
constexpr uint32_t kDemoteWindow = 500;        // calls per p95 sample
constexpr uint32_t kDemoteP95Us10 = 200;       // 20 us, in 0.1 us units

struct Info {
    char name[64];
    uint32_t version, demoteEvery;
    bool inReplayCompare, demoted, faulted;
    uint32_t demotedAt, faultedAt;             // ticks
    uint32_t lastP95Us10;                      // p95 of the last full window, 0.1 us units
    uint64_t calls;
};
size_t List(Info* out, size_t max);            // name order; any thread
size_t Count();
uint64_t ListHash();                           // FNV over the names and versions, in order: equal lists, equal hash
std::string Describe(const Info& i);           // "hashed every tick", "hashed every 10 ticks (demoted at ...)", ...
std::string NoteJson();                        // the contributor list with demotions and faults, for NOTE chunks

// Per-contributor hashes of the last kRingTicks ticks, for COMPS and reports.
struct Entry {
    char name[64];
    uint64_t hash;
    bool computed;                             // false: not due this tick (demoted), faulted or added later
};
size_t HashesAt(uint32_t tick, Entry* out, size_t max);  // 0 when the tick is not in the ring
// The mods value of a tick, and the same over the contributors with inReplayCompare only.
bool ModsAt(uint32_t tick, uint64_t* all, uint64_t* replay);

void ResetSession();                           // session begin: clears the rings
void SetClockForTest(int64_t (*qpc)(), int64_t freq);
}  // namespace melange::wormsign::contrib
