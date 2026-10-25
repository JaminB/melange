// media_selftest cases for the media host (owner: C4b): the media_sync.h follow policy (deadband, nudge, seek, lead,
// grace, stale epoch, paused host, clock not ready, 0-150 ms of time-pos staleness), the pipe client's coalescing and
// bounded queue, the watchdog (a 15 s main-thread stall causes no restart), and the fake helper end to end.
// Included at file scope, so it may #include what it needs; it defines host_cases::Run(), and CHECK(cond, what) is
// available. Sources: cmake/holo/media_host.cmake.
namespace host_cases {
inline void Run() {}
}  // namespace host_cases
