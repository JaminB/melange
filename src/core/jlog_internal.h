#pragma once
// Internal wiring between the Logging module (jlog_adapters.cpp) and the writer core (jlog.cpp).
// NOT part of the frozen public contract (that is melange/jlog.h); other components must never include this.
#include <cstddef>
#include <cstdint>
#include <string>

namespace melange::jlog::internal {

struct Options {
    std::wstring rootOverride;    // [Logging] Dir=; empty means "use the Documents default"
    std::wstring fallbackRoot;    // "<game>\Melange\logs", used if rootOverride/default isn't writable
    std::string levelsSpec = "*:info";  // "*:info,event:info,engine:info"
    uint32_t maxFileMB = 32;
    uint32_t maxSessions = 20;
    uint32_t maxTotalMB = 512;
    size_t tailCapacity = 5000;
};

// Starts the session folder, the writer thread and the level filter. Call exactly once, before anything calls
// melange::jlog::Rec(...). Returns false only if even the fallback root could not be made writable (logging is then
// inert: Enabled() returns false for everything and Rec() is a cheap no-op).
bool Init(const Options& opt);

// Test-only: stops the writer thread after a final flush and resets all state so Init() can run again in the
// same process. The plugin itself never calls this (the process just exits).
void ShutdownForTests();

// p95 of the main-thread record cost (Rec construction to end of Emit) in microseconds, and the sample count.
double EmitP95Us(uint64_t* samples);

}  // namespace melange::jlog::internal
