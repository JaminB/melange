#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "melange/wormsign.h"
#include "wormsign/detector_core.h"

// The desync detector: tick hashes exchanged with lobby members that advertise the exchange, the first differing
// tick reported (toast, log, bundle), never any action on the match.
namespace melange::wormsign::detector {
enum class OnDesync : uint8_t { Report, BundleOnly };
struct Options {
    bool exchange = true;
    OnDesync onDesync = OnDesync::Report;
};
bool Install(const Options& opt);   // the Wormsign module, after the tick clock
void Uninstall();

// Delivers a divergence to OnDivergence observers, the log, jlog and client Lua (wum.wormsign.onDivergence).
// Main thread. The replay player reports its own divergences through this too.
void Raise(const Divergence& d);

// Optional sources, set by the features that own the data; without them the detector exchanges less detail.
using ContribNamesFn = void (*)(std::vector<wire::ContribName>* out);          // in hashing order
using ContribHashesFn = bool (*)(uint32_t tick, std::vector<uint64_t>* out);  // per contributor, >= 512 ticks back
using DetailFn = bool (*)(uint32_t tick, std::string* json);                // the tick's detail record as JSON
using RecordingFn = bool (*)(uint32_t serial, std::wstring* path);           // flushes that match's .wsr
void SetContribSource(ContribNamesFn names, ContribHashesFn hashes);
void SetDetailSource(DetailFn fn);
void SetRecordingSource(RecordingFn fn);

using detect::PeerState;
using detect::PeerStateName;
using detect::PeerStatus;
int Peers(PeerStatus* out, int max);  // main thread
std::wstring LastBundle();            // "" before the first
std::wstring ReplaysDir();            // Documents\Melange\replays
}  // namespace melange::wormsign::detector
