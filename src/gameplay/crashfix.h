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
}  // namespace melange::crashfix
