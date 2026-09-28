#pragma once
#include <cstdint>

// Per (id, caller): the first 5 are logged, then one summary every 10 s; `budget` caps records per second.
namespace melange::gldebug::logic {
struct Entry {
    uint64_t total = 0;
    uint64_t sincePeriodic = 0;
    uint64_t lastPeriodicMs = 0;
};

struct Decision {
    bool log;
    uint64_t since;
};

constexpr uint64_t kPeriodMs = 10000;
constexpr int kFirstFive = 5;

inline Decision Decide(Entry& e, int& budget, uint64_t nowMs) {
    ++e.total;
    bool firstFive = e.total <= kFirstFive;
    bool periodicDue = !firstFive && nowMs - e.lastPeriodicMs >= kPeriodMs;
    if ((!firstFive && !periodicDue) || budget <= 0) {
        ++e.sincePeriodic;
        return {false, e.sincePeriodic};
    }
    --budget;
    uint64_t since = e.sincePeriodic;
    e.sincePeriodic = 0;
    if (periodicDue) e.lastPeriodicMs = nowMs;
    return {true, since};
}
}  // namespace melange::gldebug::logic
