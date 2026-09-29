#pragma once
#include <atomic>

// The last call into another DLL that one of our hooks saw return (a static name), for reports that need to know
// what ran last, such as the FPU watch. Cheap enough for per-frame hooks.
namespace melange::dllcall {
inline std::atomic<const char*> g_last{"none"};
inline void Note(const char* what) { g_last.store(what, std::memory_order_relaxed); }
inline const char* Last() { return g_last.load(std::memory_order_relaxed); }
}  // namespace melange::dllcall
