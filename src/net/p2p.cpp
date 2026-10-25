#include "net/p2p.h"

#include <windows.h>

#include "net/steam.h"

namespace melange::net::p2p {
namespace {
// ISteamNetworking005: 0 SendP2PPacket, 1 IsP2PPacketAvailable, 2 ReadP2PPacket, 6 GetP2PSessionState.
using Send_t = bool(__thiscall*)(void*, uint64_t, const void*, uint32_t, int, int);
using Avail_t = bool(__thiscall*)(void*, uint32_t*, int);
using Read_t = bool(__thiscall*)(void*, void*, uint32_t, uint32_t*, uint64_t*, int);
using State_t = bool(__thiscall*)(void*, uint64_t, steam::P2PSessionState*);
constexpr uint32_t kMaxRead = 1u << 20;
constexpr size_t kStackBuf = 2048;

void* Networking() {
    static void* (__cdecl * fn)() = nullptr;
    if (!fn) {
        HMODULE s = GetModuleHandleW(L"steam_api.dll");
        fn = s ? reinterpret_cast<void*(__cdecl*)()>(GetProcAddress(s, "SteamNetworking")) : nullptr;
    }
    return fn ? fn() : nullptr;
}
}  // namespace

bool Available() { return Networking() != nullptr; }

bool Send(uint64_t to, const std::vector<uint8_t>& packet, SendType type, int channel) {
    void* n = Networking();
    if (!n || !to || packet.empty() || channel == 0) return false;
    void** vt = *static_cast<void***>(n);
    return reinterpret_cast<Send_t>(vt[0])(n, to, packet.data(), static_cast<uint32_t>(packet.size()),
                                           static_cast<int>(type), channel);
}

int Drain(int channel, size_t maxBytes, RecvFn fn, void* user, int max, uint64_t* oversized) {
    void* n = Networking();
    if (!n || channel == 0) return 0;
    void** vt = *static_cast<void***>(n);
    uint8_t buf[kStackBuf];
    const size_t cap = maxBytes < sizeof buf ? maxBytes : sizeof buf;
    int got = 0;
    for (uint32_t size = 0; got < max && reinterpret_cast<Avail_t>(vt[1])(n, &size, channel); ++got) {
        uint64_t from = 0;
        uint32_t read = 0;
        if (size > cap) {
            std::vector<uint8_t> big((size < kMaxRead ? size : kMaxRead));
            if (!reinterpret_cast<Read_t>(vt[2])(n, big.data(), static_cast<uint32_t>(big.size()), &read, &from, channel))
                break;
            if (oversized) ++*oversized;
            continue;
        }
        if (!reinterpret_cast<Read_t>(vt[2])(n, buf, static_cast<uint32_t>(cap), &read, &from, channel)) break;
        if (read <= cap) fn(from, buf, read, user);
    }
    return got;
}

bool SessionActive(uint64_t member) {
    void* n = Networking();
    if (!n || !member) return false;
    void** vt = *static_cast<void***>(n);
    steam::P2PSessionState s{};
    if (!reinterpret_cast<State_t>(vt[6])(n, member, &s)) return false;  // false: no session with that user at all
    return s.connectionActive != 0;
}
}  // namespace melange::net::p2p
