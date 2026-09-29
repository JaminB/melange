#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// The FPU watch: the main thread's x87 control word and MXCSR at every tick end. The game sets 0x027f once at
// start-up and never again; a DLL that changes it would change float results unnoticed.
namespace melange::wormsign::fpu {
constexpr uint16_t kExpectedCw = 0x027f;

void Tick(uint32_t serial, uint32_t tick, uint16_t cw);   // main thread, at tick end; reads MXCSR itself
void Observe(uint32_t serial, uint32_t tick, uint16_t cw, uint32_t mxcsr);  // the same with MXCSR given (tests)
constexpr uint32_t kMxcsrFlags = 0x3f;  // sticky exception flags: ignored when comparing

struct Event {
    uint32_t serial, tick;
    uint16_t cw, prevCw;
    uint32_t mxcsr, prevMxcsr;
    char lastCall[40];
};
size_t Events(Event* out, size_t max);   // changes since start-up, oldest first (the first 64 are kept)
uint16_t Cw();                           // last seen
uint32_t Mxcsr();
uint32_t Changes();                      // changes seen, including those past the kept 64
std::string NoteJson();                  // for NOTE chunks
void ResetForTest();
}  // namespace melange::wormsign::fpu
