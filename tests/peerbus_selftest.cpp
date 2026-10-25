// Offline self-test for the peer bus: wire encode and decode, fragmentation and reassembly with loss and reordering,
// the rate limiter, the NTP min-RTT offset, the gates (no mlg.peer, version mismatch, no session, lobbyKey change)
// and a malformed-packet fuzz.
// Owner: C3 (Melange 0.12.0). Placeholder from C0 so the target exists. Exit code 0 = all passed.
#include <cstdio>

int main() {
    printf("peerbus_selftest: 0 passed, 0 failed (no cases yet)\n");
    return 0;
}
