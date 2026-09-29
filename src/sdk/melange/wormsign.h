#pragma once
#include <cstddef>
#include <cstdint>
// Wormsign: the sim's tick clock, per-tick state hashes, match recordings, replays and desync detection.
namespace melange::wormsign {
constexpr uint32_t kTickMs = 20;
constexpr uint32_t kEngineHashVersion = 1;   // the engine component set; a change is a new version

bool Enabled();                  // [Wormsign] Enabled and the #1077 prologue checks passed
bool InMatch();                  // a Wormsign match session is open (the match VM exists and ticks run)
uint32_t MatchSerial();          // +1 per session
uint32_t Tick();                 // last completed tick b (logic time 20*b); 0 before the first
uint32_t LogicTimeMs();          // TM+0x38

enum Comp : uint8_t { kTimeRng, kTurn, kWorms, kTasks, kProjectiles, kTeams, kEngineComps,
                      kRng2 = kEngineComps };  // kRng2: diagnostic, never in the hash
struct TickHash {
    uint32_t tick;
    uint64_t engine;             // FNV over c[0..5]
    uint64_t mods;               // FNV over the contributors in registration-name order; 0 when none
    uint64_t c[kEngineComps];
    uint32_t rngLogic, rng2;
    uint16_t fpucw, inputs;      // inputs: sender calls in this tick
};
bool LastTick(TickHash* out);    // any thread (seqlock copy)
bool TickAt(uint32_t tick, TickHash* out);  // from the in-match ring; main thread

// Tick-end observers: main thread, at the tick boundary, after hashing. Read-only: must not write game memory,
// post messages or draw RNG. Order: ascending `order`, then registration.
using TickEndFn = void (*)(const TickHash& h, void* user);
int OnTickEnd(TickEndFn fn, void* user, int order = 0);
void RemoveOnTickEnd(int handle);
using SessionFn = void (*)(bool begin, uint32_t serial, void* user);  // begin: before tick 1; end: after the last
int OnSession(SessionFn fn, void* user);
void RemoveOnSession(int handle);

// Hash contributors. Called at every tick end in a match, main thread, in ascending `name` order.
// Rules: feed raw stored bytes (never floats you computed), deterministic across machines, no allocation,
// <= 5 us. Over 20 us p95 for 500 ticks demotes it to every `demoteEvery` ticks (logged, shown in reports).
class Hasher {
  public:
    virtual void Bytes(const void* p, size_t n) = 0;
    template <class T> void Val(const T& v) { Bytes(&v, sizeof v); }
  protected:
    ~Hasher() = default;
};
using ContribFn = void (*)(Hasher& h, uint32_t tick, void* user);
struct ContribOptions { uint32_t version = 1; uint32_t demoteEvery = 10; bool inReplayCompare = true; };
int AddContributor(const char* name, ContribFn fn, void* user, const ContribOptions& opt = {});  // "mod.<id>.<x>" or module name
void RemoveContributor(int handle);

// Divergence reports (replays and peers).
enum class Source : uint8_t { Replay, Peer };
struct Divergence {
    Source source;
    uint32_t serial, tick;       // first differing tick
    uint64_t oursEngine, theirsEngine, oursMods, theirsMods;
    uint8_t compMask;            // bit i: engine component i differs
    char contrib[64];            // first differing contributor name, "" if none
    uint64_t peer;               // SteamID for Source::Peer
    wchar_t bundle[260];         // desync bundle path once written, else ""
};
using DivergenceFn = void (*)(const Divergence& d, void* user);  // main thread
int OnDivergence(DivergenceFn fn, void* user);
void RemoveOnDivergence(int handle);

// Recordings and replay.
struct ReplayInfo {
    wchar_t path[260];
    uint64_t bytes; uint32_t ticks, inputs, remoteInputs;
    int64_t startUnix; char exeBuild[8], melange[24], land[64];
    char contentHash16[17];      // mods::ContentId prefix, "" vanilla
    bool online, complete, pinned, flagged;   // complete: closed normally; flagged: a divergence was seen
};
int Library(ReplayInfo* out, int max);        // newest first; any thread
bool Pin(const wchar_t* path, bool pinned);   // pinned files are never pruned
enum class PlayState : uint8_t { Idle, Armed, Loading, Playing, Paused, Finished, Diverged, Failed };
struct PlayStatus { PlayState state; uint32_t tick, ticks, compared, matched; float speed; char error[128]; };
bool Arm(const wchar_t* path, char* err, size_t errLen);  // offline and not in a lobby only
void Disarm();
bool SetPaused(bool p);
bool SetSpeed(float x);          // 0.25 .. 8
bool RunTo(uint32_t tick);       // fast-forward (max speed, then pause); ticks behind -> restart required
PlayStatus Status();             // any thread
}
