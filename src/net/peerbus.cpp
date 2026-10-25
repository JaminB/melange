// The peer bus on Steam P2P channel 6 (peerbus.h): module [PeerBus].
// Placeholder from Melange 0.12.0 C0: it sends and reads nothing. C3 replaces this file.
#include "net/peerbus.h"

#include <windows.h>

#include "core/module.h"

namespace melange::peerbus {
double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

bool Clock(uint64_t, ClockInfo*) { return false; }

namespace {
class PeerBus final : public Module {
public:
    const char* Name() const override { return "PeerBus"; }
    const char* Description() const override { return "Steam P2P channel 6 for client mods (wum.peer)"; }
    int Order() const override { return 58; }  // after Handshake (57), which publishes mlg.peer
    bool Install() override { return true; }
};
}  // namespace
MELANGE_MODULE(PeerBus);
}  // namespace melange::peerbus
