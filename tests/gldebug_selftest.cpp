// Offline self-test of the KHR_debug rate limiter (render/mirage/gldebug_logic.h).
#include <cstdio>

#include "render/mirage/gldebug_logic.h"

namespace {
using melange::gldebug::logic::Decide;
using melange::gldebug::logic::Entry;

struct Ctx {
    int checks = 0, failed = 0;
    void Check(bool ok, const char* what) {
        ++checks;
        if (ok) return;
        ++failed;
        std::printf("FAIL: %s\n", what);
    }
};

void TestFirstFive(Ctx& c) {
    Entry e;
    int budget = 200;
    for (int i = 1; i <= 5; ++i) c.Check(Decide(e, budget, 1000).log, "first five occurrences are logged individually");
    c.Check(!Decide(e, budget, 1000).log, "the 6th occurrence in the same instant is not logged");
    c.Check(budget == 195, "budget is decremented once per logged record, not per occurrence");
}

void TestPeriodicSummary(Ctx& c) {
    Entry e;
    int budget = 200;
    for (int i = 0; i < 5; ++i) Decide(e, budget, 0);
    for (int i = 0; i < 100; ++i) Decide(e, budget, 5000);
    c.Check(!Decide(e, budget, 9999).log, "still silent just under 10s since the last summary");
    melange::gldebug::logic::Decision due = Decide(e, budget, 10000);
    c.Check(due.log && due.since == 101, "exactly one summary at 10s, carrying the correct backlog count");
    c.Check(!Decide(e, budget, 10001).log, "goes quiet again immediately after a summary");
}

void TestBudgetCapAcrossKeys(Ctx& c) {
    Entry keys[300];
    int budget = 200;
    int logged = 0;
    for (Entry& k : keys)
        if (Decide(k, budget, 0).log) ++logged;
    c.Check(logged == 200, "the per-second cap applies across distinct keys too, not just repeats of one key");
    c.Check(budget == 0, "budget never goes negative");
}

void Test10kBurstOneKey(Ctx& c) {
    Entry e;
    int budget = 200;
    int logged = 0, suppressed = 0;
    for (int i = 0; i < 10000; ++i) (Decide(e, budget, 0).log ? logged : suppressed)++;
    c.Check(logged <= 205, "at most ~205 records logged in that second");
    c.Check(suppressed >= 9795, "at least 9795 of the 10000 are counted as suppressed");
    c.Check(logged == 5, "one repeating key alone never exhausts the 200/s budget: only its first-five lines log");
}
}  // namespace

int main() {
    Ctx c;
    TestFirstFive(c);
    TestPeriodicSummary(c);
    TestBudgetCapAcrossKeys(c);
    Test10kBurstOneKey(c);
    std::printf("gldebug: %d/%d checks passed\n", c.checks - c.failed, c.checks);
    return c.failed ? 1 : 0;
}
