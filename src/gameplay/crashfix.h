#pragma once
#include <cstdint>

// Game-independent parts of the Fixes module, shared with tests/crashfix_selftest.cpp.
namespace melange::crashfix {
// A zeroed stand-in for a game singleton that is already destroyed. A guard swaps a null base pointer for the sink
// so the faulting instruction touches the sink instead of address 0.
struct Sink {
    alignas(16) uint8_t bytes[0x100];
};

// Points a null `reg` at the sink. Returns true when it did.
bool NullToSink(uintptr_t& reg, Sink& sink);

// "/SEPIA" is parsed at startup, before AppDataService exists, and calls appData->postProcess->SetSepia(1)
// (post-process at +0x5C, vtable slot 5, thiscall). When AppDataService or its post-process is missing,
// SepiaToStandIn points `appData` at a stand-in whose post-process records the call. Returns true when it did.
bool SepiaToStandIn(uintptr_t& appData);
bool SepiaRequested();
// Makes the recorded SetSepia(1) call on the post-process `pp`, once per instance. Returns true when it called.
bool ApplySepia(uintptr_t pp);
void ResetSepia();

// Composite tints with the "Sepia.Color" and "Sepia.LerpWeight" tweaks it looked up when the post-process was built
// (pp+0x34, pp+0x38) and draws untinted while either is null. A null one is looked up again with the game's resolver
// (cdecl, name by pointer, refcounted out slot), else filled with a holder of the shipped values.
using ResolveTweak = int(__cdecl*)(const char* const* name, uint32_t* out);
enum class Tint { Present, Resolved, Fallback };
Tint EnsureSepiaTint(uintptr_t pp, ResolveTweak colour, ResolveTweak weight);
}  // namespace melange::crashfix
