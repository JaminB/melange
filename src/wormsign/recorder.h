#pragma once
#include <cstdint>

// The rolling recorder: wires capture, rngtap, the public session observers and the writer/library together into
// one .wsr recording per match. Internal; no other code depends on this header's shape.
namespace melange::wormsign::recorder {
bool Install();               // registers the session/tick-end observers; call once, after clock::Install()
void SetRecordEnabled(bool on);   // [Wormsign] Record
void SetDetailEnabled(bool on);   // [Wormsign] RecordDetail (no detail source until A lands; accepted, unused)
struct Stats {
    uint64_t chunksQueued, chunksDropped;
    uint64_t bytesWritten;
    uint32_t recordingsWritten;
};
Stats GetStats();
}  // namespace melange::wormsign::recorder
