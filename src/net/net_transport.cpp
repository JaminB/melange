// NetTransport: fixes the game's hand-rolled reliable-UDP layer on top of Steam P2P (XSteamConnection).
//
// Bug (see docs/netcode.md): the retry window base XSteamConnection+0x2c is set to 1 in the ctor and never
// advanced, so only sequence numbers 1 and 2 are ever retransmitted on a timer. After that a lost packet is
// only recovered if a *later* packet makes the receiver notice the gap. When the last packet before a
// waiting phase is lost (load-complete, turn hand-off, time sync...) both machines wait on each other forever:
// the match freezes, and eventually the channel times out -> "This session is no longer available".
//
// Fix (sender side, compatible with unmodified peers - wire protocol unchanged):
//   on every cumulative ACK, advance +0x2c past the acked sequence and keep the retry timer armed while
//   anything is still unacknowledged, so the two oldest unacked packets are resent every 250 ms.
// Fix (receiver side): a duplicate of an already-delivered packet is re-ACKed (the game drops it silently),
//   so a lost ACK can't make the peer retransmit forever.
#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <vector>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "net/net.h"

namespace {
using wf::wum::Read;

constexpr uintptr_t kOnAck = 0x785b69;       // __thiscall XSteamConnection::OnAck(uint ackedSeq), ret 4
constexpr uintptr_t kOnPacket = 0x785d9a;    // __thiscall XSteamConnection::OnPacket(uint* pkt, uint len), ret 8
constexpr uintptr_t kClockMs = 0x63a903;     // uint __cdecl GetTimeMs()

// XSteamConnection fields
constexpr uintptr_t kAddr = 0x14;       // XSteamAddress* (CSteamID at +0x14/+0x18)
constexpr uintptr_t kUnacked = 0x1c;    // list of sent-unacked packets: node {uint* buf, uint len, node* next}
constexpr uintptr_t kSendSeq = 0x24;
constexpr uintptr_t kRecvSeq = 0x28;
constexpr uintptr_t kRetryBase = 0x2c;  // first seq the retry timer resends (stuck at 1 in the original game)
constexpr uintptr_t kRetryArmed = 0x3c;
constexpr uintptr_t kRetryAt = 0x40;
constexpr uint32_t kRetryMs = 250;

std::vector<SafetyHookMid> g_hooks;
bool g_logEachAck = false;
std::atomic<uint32_t> g_retryFixes{0}, g_dupAcks{0};

uint32_t Now() { return reinterpret_cast<uint32_t(__cdecl*)()>(kClockMs)(); }

bool HasUnackedAfter(uintptr_t conn, uint32_t seq) {
    uintptr_t node = Read<uint32_t>(conn + kUnacked);
    for (int guard = 0; node && guard < 4096; ++guard) {
        uint32_t hdr = Read<uint32_t>(Read<uint32_t>(node));
        if ((hdr >> 2) > seq) return true;
        node = Read<uint32_t>(node + 8);
    }
    return false;
}

void OnAck(safetyhook::Context& c) {
    uintptr_t conn = c.ecx;
    uint32_t acked = Read<uint32_t>(c.esp + 4);
    uint32_t base = Read<uint32_t>(conn + kRetryBase);
    if (acked + 1 > base && acked <= Read<uint32_t>(conn + kSendSeq)) {
        wf::wum::WriteInt(conn + kRetryBase, static_cast<int32_t>(acked + 1));
        g_retryFixes++;
    }
    // Keep the retransmit timer running while packets newer than this ACK are still outstanding.
    if (!Read<uint8_t>(conn + kRetryArmed) && HasUnackedAfter(conn, acked)) {
        wf::wum::WriteByte(conn + kRetryArmed, 1);
        wf::wum::WriteInt(conn + kRetryAt, static_cast<int32_t>(Now() + kRetryMs));
    }
    if (g_logEachAck)
        WF_TRACE("[transport] conn %08x ack %u (send %u) retryBase %u -> %u", static_cast<unsigned>(conn), acked,
                 Read<uint32_t>(conn + kSendSeq), base, Read<uint32_t>(conn + kRetryBase));
}

using Send_t = bool(__thiscall*)(void*, uint64_t, const void*, uint32_t, int, int);

void OnPacket(safetyhook::Context& c) {
    uintptr_t conn = c.ecx;
    uint32_t hdr = Read<uint32_t>(Read<uint32_t>(c.esp + 4));
    if (hdr & 3) return;  // ACK or resend-request, not data
    uint32_t seq = hdr >> 2, delivered = Read<uint32_t>(conn + kRecvSeq);
    if (seq == 0 || seq > delivered) return;  // new data: the game handles it
    // Duplicate of something we already delivered: our ACK was probably lost. Re-ACK cumulatively.
    uintptr_t addr = Read<uint32_t>(conn + kAddr);
    uint64_t peer = static_cast<uint64_t>(Read<uint32_t>(addr + 0x18)) << 32 | Read<uint32_t>(addr + 0x14);
    HMODULE api = GetModuleHandleW(L"steam_api.dll");
    auto accessor = api ? reinterpret_cast<void*(__cdecl*)()>(GetProcAddress(api, "SteamNetworking")) : nullptr;
    void* net = accessor ? accessor() : nullptr;
    if (!net || !peer) return;
    uint32_t ack = delivered << 2 | 1;
    auto send = reinterpret_cast<Send_t>((*static_cast<void***>(net))[0]);
    send(net, peer, &ack, 4, 0, 0);
    if (g_dupAcks++ < 50)
        WF_INFO("[transport] duplicate seq %u from %llu (delivered %u) -> re-ACK", seq,
                static_cast<unsigned long long>(peer), delivered);
}

class NetTransport final : public wf::Module {
public:
    const char* Name() const override { return "NetTransport"; }
    const char* Description() const override { return "fixes lost-packet recovery in the game's P2P reliability layer"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 21; }

    bool Install() override {
        g_logEachAck = Bool("LogAcks", false);
        bool ok = true;
        if (Bool("FixRetransmit", true)) {
            // push esi / lea esi,[ecx+0x1c]
            if (!wf::mem::Expect(kOnAck, {0x56, 0x8d, 0x71, 0x1c})) return false;
            auto h = safetyhook::create_mid(kOnAck, &OnAck);
            ok &= static_cast<bool>(h);
            g_hooks.push_back(std::move(h));
        }
        if (Bool("ReAckDuplicates", true)) {
            if (!wf::mem::Expect(kOnPacket, {0x55, 0x8b, 0xec, 0x56, 0x57})) return false;
            auto h = safetyhook::create_mid(kOnPacket, &OnPacket);
            ok &= static_cast<bool>(h);
            g_hooks.push_back(std::move(h));
        }
        return ok;
    }

    void Uninstall() override {
        WF_INFO("[transport] session totals: %u retry-window advances, %u duplicate re-ACKs", g_retryFixes.load(),
                g_dupAcks.load());
        g_hooks.clear();
    }
};
}  // namespace

WUMFIX_MODULE(NetTransport);
