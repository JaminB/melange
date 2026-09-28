#pragma once
// Internal wiring between the Logging module (jlog_adapters.cpp) and the writer (jlog.cpp). Mods use melange/jlog.h.
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

// Call exactly once, before any Rec(). Returns false if no root is writable; logging is then an inert no-op.
bool Init(const Options& opt);

// Test only: flushes, stops the writer and resets state so Init() can run again.
void ShutdownForTests();

// p95 main-thread cost of one record (Rec construction to end of Emit), in microseconds.
double EmitP95Us(uint64_t* samples);

}  // namespace melange::jlog::internal
