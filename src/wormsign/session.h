#pragma once
#include <cstdint>

#include "melange/wormsign.h"
#include "wormsign/hash_engine.h"

// The match session behind melange/wormsign.h: open/close, per-tick hashing, the ring and the observers.
// Main thread unless marked.
namespace melange::wormsign::session {
void Begin();
void End(const char* reason);
const char* EndReason();                        // why the last session ended (a string literal); main thread
bool Open();                                   // any thread
void EndTick(uint32_t bucket, const PoppedTask& popped);
void CountInput();                             // one sender call in the current tick (the recorder's capture)
void SetEnabled(bool on);                      // the module passed its checks
struct Cost { uint64_t ticks; double hashUsMean, hashUsP95, hashUsMax, tickUsMean; };
Cost GetCost();                                // since the last ResetCost
void ResetCost();
// OnSession/OnTickEnd with a name for the fault log ("[wormsign] session observer 1 (wormsign recorder) faulted").
// The public ones are named after the module their function lives in. `name` must outlive the observer.
int OnSessionNamed(SessionFn fn, void* user, const char* name);
int OnTickEndNamed(TickEndFn fn, void* user, int order, const char* name);
}  // namespace melange::wormsign::session
