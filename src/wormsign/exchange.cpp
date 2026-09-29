#include "wormsign/exchange.h"

#include <windows.h>

#include "wormsign/exchange_wire.h"

namespace melange::wormsign::exchange {
namespace {
// ISteamNetworking005: 0 SendP2PPacket, 1 IsP2PPacketAvailable, 2 ReadP2PPacket.
using Send_t = bool(__thiscall*)(void*, uint64_t, const void*, uint32_t, int, int);
using Avail_t = bool(__thiscall*)(void*, uint32_t*, int);
using Read_t = bool(__thiscall*)(void*, void*, uint32_t, uint32_t*, uint64_t*, int);
constexpr int kUnreliableNoDelay = 1, kReliable = 2;
constexpr uint32_t kMaxRead = 1u << 20;

Counters g_c{};

void* Networking() {
    static void* (__cdecl * fn)() = nullptr;
    if (!fn) {
        HMODULE s = GetModuleHandleW(L"steam_api.dll");
        fn = s ? reinterpret_cast<void*(__cdecl*)()>(GetProcAddress(s, "SteamNetworking")) : nullptr;
    }
    return fn ? fn() : nullptr;
}
}  // namespace

bool Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable) {
    void* n = Networking();
    if (!n || !to || packet.empty() || packet.size() > wire::kMaxPacket) return false;
    void** vt = *static_cast<void***>(n);
    const bool ok = reinterpret_cast<Send_t>(vt[0])(n, to, packet.data(), static_cast<uint32_t>(packet.size()),
                                                    reliable ? kReliable : kUnreliableNoDelay, wire::kChannel);
    if (ok) {
        ++g_c.sent;
        g_c.bytesSent += packet.size();
    } else {
        ++g_c.sendFailed;
    }
    return ok;
}

int Drain(RecvFn fn, void* user, int max) {
    void* n = Networking();
    if (!n) return 0;
    void** vt = *static_cast<void***>(n);
    uint8_t buf[wire::kMaxPacket + 100];
    int got = 0;
    for (uint32_t size = 0; got < max && reinterpret_cast<Avail_t>(vt[1])(n, &size, wire::kChannel); ++got) {
        uint64_t from = 0;
        uint32_t read = 0;
        if (size > sizeof buf) {
            std::vector<uint8_t> big((size < kMaxRead ? size : kMaxRead));
            if (!reinterpret_cast<Read_t>(vt[2])(n, big.data(), static_cast<uint32_t>(big.size()), &read, &from, wire::kChannel))
                break;
            ++g_c.oversized;
            continue;
        }
        if (!reinterpret_cast<Read_t>(vt[2])(n, buf, sizeof buf, &read, &from, wire::kChannel)) break;
        ++g_c.received;
        g_c.bytesReceived += read;
        if (read <= sizeof buf) fn(from, buf, read, user);
    }
    return got;
}

Counters Stats() { return g_c; }
}  // namespace melange::wormsign::exchange
