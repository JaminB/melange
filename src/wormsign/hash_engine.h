#pragma once
#include <cstdint>

#include "melange/wormsign.h"

namespace melange::wormsign {
// The task being dispatched when a tick ends: already popped from its queue, hashed as that queue's front.
struct PoppedTask {
    int cat = -1;
    uint32_t time = 0;
    uintptr_t obj = 0;
};
namespace detail {
struct DetailRec;
}
// Fills tick, engine, c[], rngLogic and rng2 of `out` for the tick ending at logic time `t`. Pure reads, fault
// guarded, no allocation; main thread. `rec`, when given (cleared), also gets the bytes that were hashed.
void ComputeEngine(uint32_t t, TickHash* out, const PoppedTask& popped = {}, detail::DetailRec* rec = nullptr);

// The active logical camera (detail::CameraDetail) as a hash contributor, in the mods hash rather than the engine
// hash: peers compare the mods hash only when their contributor lists match, so a peer without it (an older
// Melange, or [Wormsign] HashCamera=0) still compares engine hashes with us and never sees a false desync.
constexpr const char* kCameraContrib = "melange.camera";
constexpr uint32_t kCameraContribVersion = 1;
bool AddCameraContributor();     // main thread, outside a tick
void RemoveCameraContributor();

constexpr uint64_t kFnvBasis = 1469598103934665603ULL;
inline uint64_t Fnv(const void* p, size_t n, uint64_t h = kFnvBasis) {
    auto b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}
template <class T>
uint64_t FnvV(const T& v, uint64_t h = kFnvBasis) { return Fnv(&v, sizeof v, h); }
}  // namespace melange::wormsign
