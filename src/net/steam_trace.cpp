// SteamTrace: logs everything the game does with Steam lobbies, P2P networking and callbacks.
// Hooks the steam_api accessor imports, then the ISteamNetworking005 / ISteamMatchmaking008 vtables.
#include <windows.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "core/debug.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "net/steam.h"

namespace {
using melange::steam::SteamAPICall;
using melange::steam::SteamID;

// ---------------------------------------------------------------- state
std::mutex g_lock;
bool g_verbose = false;
int g_summarySeconds = 5;

struct Counter {
    uint64_t packets = 0, bytes = 0;
};
std::map<std::string, Counter> g_counters;  // "send ch0", "recv sock", ...
bool g_countersDirty = false;
std::set<SteamID> g_peers;                  // every remote we exchanged P2P traffic with

void Count(const char* dir, int channel, uint32_t bytes) {
    char key[32];
    snprintf(key, sizeof(key), "%s ch%d", dir, channel);
    std::lock_guard lk(g_lock);
    auto& c = g_counters[key];
    c.packets++;
    c.bytes += bytes;
    g_countersDirty = true;
}

std::string Id(SteamID id) {
    char b[32];
    snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(id));
    return b;
}

// Caller of a hooked Steam method, to map traffic back to game code.
#define CALLER melange::game::DescribeAddress(reinterpret_cast<uintptr_t>(_ReturnAddress())).c_str()

// ---------------------------------------------------------------- ISteamNetworking005
void** g_netVt = nullptr;
void* g_netObj = nullptr;
using Send_t = bool(__thiscall*)(void*, SteamID, const void*, uint32_t, int, int);
using Read_t = bool(__thiscall*)(void*, void*, uint32_t, uint32_t*, SteamID*, int);
using Id_t = bool(__thiscall*)(void*, SteamID);
using IdInt_t = bool(__thiscall*)(void*, SteamID, int);
using GetState_t = bool(__thiscall*)(void*, SteamID, melange::steam::P2PSessionState*);
using CreateListen_t = uint32_t(__thiscall*)(void*, int, uint32_t, uint16_t, bool);
using CreateP2PConn_t = uint32_t(__thiscall*)(void*, SteamID, int, int, bool);
using Destroy_t = bool(__thiscall*)(void*, uint32_t, bool);
using SendSock_t = bool(__thiscall*)(void*, uint32_t, void*, uint32_t, bool);
using RetrSock_t = bool(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t*);
using Retr_t = bool(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t*, uint32_t*);

Send_t oSend;
Read_t oRead;
Id_t oAccept, oCloseSession;
IdInt_t oCloseChannel;
CreateListen_t oCreateListen;
CreateP2PConn_t oCreateP2PConn;
Destroy_t oDestroySocket, oDestroyListen;
SendSock_t oSendSock;
RetrSock_t oRetrSock;
Retr_t oRetr;

bool __fastcall hkSend(void* self, void*, SteamID to, const void* data, uint32_t n, int type, int ch) {
    bool r = oSend(self, to, data, n, type, ch);
    Count("send", ch, n);
    { std::lock_guard lk(g_lock); g_peers.insert(to); }
    if (g_verbose) {
        char t[96];
        snprintf(t, sizeof(t), "P2P send -> %s ch%d type%d ok=%d", Id(to).c_str(), ch, type, r);
        melange::log::HexDump(t, data, n, 24);
    }
    if (!r) WF_WARN("SendP2PPacket FAILED to %s ch%d (%u bytes) from %s", Id(to).c_str(), ch, n, CALLER);
    return r;
}

bool __fastcall hkRead(void* self, void*, void* dest, uint32_t cub, uint32_t* size, SteamID* from, int ch) {
    bool r = oRead(self, dest, cub, size, from, ch);
    if (r && size) {
        Count("recv", ch, *size);
        if (g_verbose) {
            char t[96];
            snprintf(t, sizeof(t), "P2P recv <- %s ch%d", from ? Id(*from).c_str() : "?", ch);
            melange::log::HexDump(t, dest, *size, 24);
        }
    }
    return r;
}

bool __fastcall hkAccept(void* self, void*, SteamID id) {
    bool r = oAccept(self, id);
    WF_INFO("AcceptP2PSessionWithUser(%s) = %d  [%s]", Id(id).c_str(), r, CALLER);
    return r;
}

bool __fastcall hkCloseSession(void* self, void*, SteamID id) {
    bool r = oCloseSession(self, id);
    WF_INFO("CloseP2PSessionWithUser(%s) = %d  [%s]", Id(id).c_str(), r, CALLER);
    return r;
}

bool __fastcall hkCloseChannel(void* self, void*, SteamID id, int ch) {
    bool r = oCloseChannel(self, id, ch);
    WF_INFO("CloseP2PChannelWithUser(%s, ch%d) = %d  [%s]", Id(id).c_str(), ch, r, CALLER);
    return r;
}

uint32_t __fastcall hkCreateListen(void* self, void*, int vport, uint32_t ip, uint16_t port, bool relay) {
    uint32_t s = oCreateListen(self, vport, ip, port, relay);
    WF_INFO("CreateListenSocket(vport=%d ip=%08x port=%u relay=%d) = %u  [%s]", vport, ip, port, relay, s, CALLER);
    return s;
}

uint32_t __fastcall hkCreateP2PConn(void* self, void*, SteamID target, int vport, int timeout, bool relay) {
    uint32_t s = oCreateP2PConn(self, target, vport, timeout, relay);
    WF_INFO("CreateP2PConnectionSocket(%s vport=%d timeout=%d relay=%d) = %u  [%s]", Id(target).c_str(), vport, timeout,
            relay, s, CALLER);
    return s;
}

bool __fastcall hkDestroySocket(void* self, void*, uint32_t s, bool notify) {
    bool r = oDestroySocket(self, s, notify);
    WF_INFO("DestroySocket(%u notify=%d) = %d  [%s]", s, notify, r, CALLER);
    return r;
}

bool __fastcall hkDestroyListen(void* self, void*, uint32_t s, bool notify) {
    bool r = oDestroyListen(self, s, notify);
    WF_INFO("DestroyListenSocket(%u notify=%d) = %d  [%s]", s, notify, r, CALLER);
    return r;
}

bool __fastcall hkSendSock(void* self, void*, uint32_t s, void* data, uint32_t n, bool reliable) {
    bool r = oSendSock(self, s, data, n, reliable);
    Count("send sock", static_cast<int>(s), n);
    if (g_verbose) {
        char t[64];
        snprintf(t, sizeof(t), "sock send %u rel=%d ok=%d", s, reliable, r);
        melange::log::HexDump(t, data, n, 24);
    }
    if (!r) WF_WARN("SendDataOnSocket FAILED sock=%u (%u bytes) from %s", s, n, CALLER);
    return r;
}

bool __fastcall hkRetrSock(void* self, void*, uint32_t s, void* dest, uint32_t cub, uint32_t* size) {
    bool r = oRetrSock(self, s, dest, cub, size);
    if (r && size) Count("recv sock", static_cast<int>(s), *size);
    return r;
}

bool __fastcall hkRetr(void* self, void*, uint32_t listen, void* dest, uint32_t cub, uint32_t* size, uint32_t* sock) {
    bool r = oRetr(self, listen, dest, cub, size, sock);
    if (r && size) Count("recv listen", sock ? static_cast<int>(*sock) : -1, *size);
    return r;
}

void HookNetworking(void* obj) {
    if (!obj) return;
    std::lock_guard lk(g_lock);
    void** vt = *static_cast<void***>(obj);
    if (vt == g_netVt) return;
    g_netVt = vt;
    g_netObj = obj;
    auto H = [&](int slot, void* hook, void* orig) { melange::mem::HookVTable(obj, slot, hook, static_cast<void**>(orig)); };
    H(0, reinterpret_cast<void*>(&hkSend), &oSend);
    H(2, reinterpret_cast<void*>(&hkRead), &oRead);
    H(3, reinterpret_cast<void*>(&hkAccept), &oAccept);
    H(4, reinterpret_cast<void*>(&hkCloseSession), &oCloseSession);
    H(5, reinterpret_cast<void*>(&hkCloseChannel), &oCloseChannel);
    H(8, reinterpret_cast<void*>(&hkCreateListen), &oCreateListen);
    H(9, reinterpret_cast<void*>(&hkCreateP2PConn), &oCreateP2PConn);
    H(11, reinterpret_cast<void*>(&hkDestroySocket), &oDestroySocket);
    H(12, reinterpret_cast<void*>(&hkDestroyListen), &oDestroyListen);
    H(13, reinterpret_cast<void*>(&hkSendSock), &oSendSock);
    H(15, reinterpret_cast<void*>(&hkRetrSock), &oRetrSock);
    H(17, reinterpret_cast<void*>(&hkRetr), &oRetr);
    WF_INFO("SteamTrace: hooked ISteamNetworking005 vtable %p", static_cast<void*>(vt));
}

// ---------------------------------------------------------------- ISteamMatchmaking008
void** g_mmVt = nullptr;
using Call0_t = SteamAPICall(__thiscall*)(void*);
using CreateLobby_t = SteamAPICall(__thiscall*)(void*, int, int);
using JoinLobby_t = SteamAPICall(__thiscall*)(void*, SteamID);
using LeaveLobby_t = void(__thiscall*)(void*, SteamID);
using IdId_t = bool(__thiscall*)(void*, SteamID, SteamID);
using SetData_t = bool(__thiscall*)(void*, SteamID, const char*, const char*);
using SetMemberData_t = void(__thiscall*)(void*, SteamID, const char*, const char*);
using ChatMsg_t = bool(__thiscall*)(void*, SteamID, const void*, int);
using IdIntB_t = bool(__thiscall*)(void*, SteamID, int);
using IdBool_t = bool(__thiscall*)(void*, SteamID, bool);

Call0_t oRequestLobbyList;
CreateLobby_t oCreateLobby;
JoinLobby_t oJoinLobby;
LeaveLobby_t oLeaveLobby;
IdId_t oInvite, oSetOwner;
SetData_t oSetLobbyData;
SetMemberData_t oSetMemberData;
ChatMsg_t oChatMsg;
IdIntB_t oSetType;
IdBool_t oSetJoinable;

SteamAPICall __fastcall hkRequestLobbyList(void* self, void*) {
    auto c = oRequestLobbyList(self);
    WF_INFO("RequestLobbyList() = call %llu  [%s]", c, CALLER);
    return c;
}
SteamAPICall __fastcall hkCreateLobby(void* self, void*, int type, int max) {
    auto c = oCreateLobby(self, type, max);
    WF_INFO("CreateLobby(type=%d max=%d) = call %llu  [%s]", type, max, c, CALLER);
    return c;
}
SteamAPICall __fastcall hkJoinLobby(void* self, void*, SteamID lobby) {
    auto c = oJoinLobby(self, lobby);
    WF_INFO("JoinLobby(%s) = call %llu  [%s]", Id(lobby).c_str(), c, CALLER);
    return c;
}
void __fastcall hkLeaveLobby(void* self, void*, SteamID lobby) {
    WF_INFO("LeaveLobby(%s)  [%s]", Id(lobby).c_str(), CALLER);
    oLeaveLobby(self, lobby);
}
bool __fastcall hkInvite(void* self, void*, SteamID lobby, SteamID user) {
    bool r = oInvite(self, lobby, user);
    WF_INFO("InviteUserToLobby(%s, %s) = %d", Id(lobby).c_str(), Id(user).c_str(), r);
    return r;
}
bool __fastcall hkSetOwner(void* self, void*, SteamID lobby, SteamID owner) {
    bool r = oSetOwner(self, lobby, owner);
    WF_INFO("SetLobbyOwner(%s, %s) = %d  [%s]", Id(lobby).c_str(), Id(owner).c_str(), r, CALLER);
    return r;
}
bool __fastcall hkSetLobbyData(void* self, void*, SteamID lobby, const char* k, const char* v) {
    bool r = oSetLobbyData(self, lobby, k, v);
    WF_INFO("SetLobbyData(%s, \"%s\" = \"%.200s\") = %d  [%s]", Id(lobby).c_str(), k ? k : "", v ? v : "", r, CALLER);
    return r;
}
void __fastcall hkSetMemberData(void* self, void*, SteamID lobby, const char* k, const char* v) {
    WF_INFO("SetLobbyMemberData(%s, \"%s\" = \"%.200s\")  [%s]", Id(lobby).c_str(), k ? k : "", v ? v : "", CALLER);
    oSetMemberData(self, lobby, k, v);
}
bool __fastcall hkChatMsg(void* self, void*, SteamID lobby, const void* body, int n) {
    bool r = oChatMsg(self, lobby, body, n);
    char t[80];
    snprintf(t, sizeof(t), "SendLobbyChatMsg(%s) = %d", Id(lobby).c_str(), r);
    melange::log::HexDump(t, body, n > 0 ? n : 0, 32);
    return r;
}
bool __fastcall hkSetType(void* self, void*, SteamID lobby, int type) {
    bool r = oSetType(self, lobby, type);
    WF_INFO("SetLobbyType(%s, %d) = %d  [%s]", Id(lobby).c_str(), type, r, CALLER);
    return r;
}
bool __fastcall hkSetJoinable(void* self, void*, SteamID lobby, bool j) {
    bool r = oSetJoinable(self, lobby, j);
    WF_INFO("SetLobbyJoinable(%s, %d) = %d  [%s]", Id(lobby).c_str(), j, r, CALLER);
    return r;
}

void HookMatchmaking(void* obj) {
    if (!obj) return;
    std::lock_guard lk(g_lock);
    void** vt = *static_cast<void***>(obj);
    if (vt == g_mmVt) return;
    g_mmVt = vt;
    auto H = [&](int slot, void* hook, void* orig) { melange::mem::HookVTable(obj, slot, hook, static_cast<void**>(orig)); };
    H(4, reinterpret_cast<void*>(&hkRequestLobbyList), &oRequestLobbyList);
    H(12, reinterpret_cast<void*>(&hkCreateLobby), &oCreateLobby);
    H(13, reinterpret_cast<void*>(&hkJoinLobby), &oJoinLobby);
    H(14, reinterpret_cast<void*>(&hkLeaveLobby), &oLeaveLobby);
    H(15, reinterpret_cast<void*>(&hkInvite), &oInvite);
    H(19, reinterpret_cast<void*>(&hkSetLobbyData), &oSetLobbyData);
    H(24, reinterpret_cast<void*>(&hkSetMemberData), &oSetMemberData);
    H(25, reinterpret_cast<void*>(&hkChatMsg), &oChatMsg);
    H(32, reinterpret_cast<void*>(&hkSetType), &oSetType);
    H(33, reinterpret_cast<void*>(&hkSetJoinable), &oSetJoinable);
    H(35, reinterpret_cast<void*>(&hkSetOwner), &oSetOwner);
    WF_INFO("SteamTrace: hooked ISteamMatchmaking008 vtable %p", static_cast<void*>(vt));
}

// ---------------------------------------------------------------- accessor + callback registration imports
using Accessor_t = void*(__cdecl*)();
Accessor_t oSteamNetworking, oSteamMatchmaking;
void* __cdecl hkSteamNetworking() {
    void* p = oSteamNetworking();
    if (p && *static_cast<void***>(p) != g_netVt) HookNetworking(p);
    return p;
}
void* __cdecl hkSteamMatchmaking() {
    void* p = oSteamMatchmaking();
    if (p && *static_cast<void***>(p) != g_mmVt) HookMatchmaking(p);
    return p;
}

using RegCb_t = void(__cdecl*)(melange::steam::CallbackBase*, int);
using UnregCb_t = void(__cdecl*)(melange::steam::CallbackBase*);
using RegCr_t = void(__cdecl*)(melange::steam::CallbackBase*, SteamAPICall);
using UnregCr_t = void(__cdecl*)(melange::steam::CallbackBase*, SteamAPICall);
RegCb_t oRegCb;
UnregCb_t oUnregCb;
RegCr_t oRegCr;
UnregCr_t oUnregCr;

void __cdecl hkRegCb(melange::steam::CallbackBase* cb, int id) {
    WF_INFO("RegisterCallback(%p %s, %d %s)  [%s]", static_cast<void*>(cb), melange::debug::RttiName(cb).c_str(), id,
            melange::steam::CallbackName(id), CALLER);
    oRegCb(cb, id);
}
void __cdecl hkUnregCb(melange::steam::CallbackBase* cb) {
    WF_INFO("UnregisterCallback(%p %s)  [%s]", static_cast<void*>(cb), melange::debug::RttiName(cb).c_str(), CALLER);
    oUnregCb(cb);
}
void __cdecl hkRegCr(melange::steam::CallbackBase* cb, SteamAPICall call) {
    WF_INFO("RegisterCallResult(%p %s, call %llu)  [%s]", static_cast<void*>(cb), melange::debug::RttiName(cb).c_str(), call,
            CALLER);
    oRegCr(cb, call);
}
void __cdecl hkUnregCr(melange::steam::CallbackBase* cb, SteamAPICall call) {
    WF_INFO("UnregisterCallResult(%p, call %llu)  [%s]", static_cast<void*>(cb), call, CALLER);
    oUnregCr(cb, call);
}

// ---------------------------------------------------------------- our own callback listeners
class Listener final : public melange::steam::CallbackBase {
public:
    explicit Listener(int id, int size) : size_(size) { melange::steam::RegisterCallback(this, id); }
    void Run(void* p) override { Log(p); }
    void Run(void* p, bool, SteamAPICall) override { Log(p); }
    int GetCallbackSizeBytes() override { return size_; }

private:
    int size_;
    void Log(void* p) {
        const auto* u64 = static_cast<const uint64_t*>(p);
        const auto* u32 = static_cast<const uint32_t*>(p);
        switch (callbackId_) {
            case melange::steam::kLobbyChatUpdate:
                WF_INFO("cb LobbyChatUpdate lobby=%s changed=%s by=%s state=0x%x", Id(u64[0]).c_str(), Id(u64[1]).c_str(),
                        Id(u64[2]).c_str(), u32[6]);
                break;
            case melange::steam::kLobbyEnter:
                WF_INFO("cb LobbyEnter lobby=%s perms=%u locked=%u response=%u", Id(u64[0]).c_str(), u32[2], u32[3] & 0xff,
                        u32[4]);
                break;
            case melange::steam::kLobbyDataUpdate:
                if (g_verbose) WF_INFO("cb LobbyDataUpdate lobby=%s member=%s", Id(u64[0]).c_str(), Id(u64[1]).c_str());
                break;
            case melange::steam::kP2PSessionRequest:
                WF_INFO("cb P2PSessionRequest from %s", Id(u64[0]).c_str());
                break;
            case melange::steam::kP2PSessionConnectFail:
                WF_WARN("cb P2PSessionConnectFail %s error=%u", Id(u64[0]).c_str(), u32[2] & 0xff);
                break;
            case melange::steam::kSocketStatus:
                WF_INFO("cb SocketStatus sock=%u listen=%u remote=%s state=%d", u32[0], u32[1], Id(u64[1]).c_str(),
                        static_cast<int>(u32[4]));
                break;
            default:
                melange::log::HexDump((std::string("cb ") + melange::steam::CallbackName(callbackId_)).c_str(), p, size_, 32);
        }
    }
};

void LogSummary() {
    std::string s;
    std::set<SteamID> peers;
    {
        std::lock_guard lk(g_lock);
        if (!g_countersDirty) return;
        g_countersDirty = false;
        for (auto& [k, c] : g_counters) {
            char b[96];
            snprintf(b, sizeof(b), " [%s: %llu pkts/%llu B]", k.c_str(), c.packets, c.bytes);
            s += b;
        }
        peers = g_peers;
    }
    WF_INFO("net totals:%s", s.c_str());
    if (g_netObj && oCloseSession) {  // session state of every peer we have talked to
        auto getState = reinterpret_cast<GetState_t>(g_netVt[6]);
        for (SteamID p : peers) {
            melange::steam::P2PSessionState st{};
            bool ok = getState(g_netObj, p, &st);
            WF_INFO("  peer %s: session=%d active=%u connecting=%u err=%u relay=%u queued=%d pkts/%d B", Id(p).c_str(), ok,
                    st.connectionActive, st.connecting, st.p2pSessionError, st.usingRelay, st.packetsQueuedForSend,
                    st.bytesQueuedForSend);
        }
    }
}

class SteamTrace final : public melange::Module {
public:
    const char* Name() const override { return "SteamTrace"; }
    const char* Description() const override { return "logs Steam lobby / P2P / callback activity"; }
    int Order() const override { return 10; }

    bool Install() override {
        g_verbose = Bool("VerbosePackets", false);
        g_summarySeconds = Int("SummarySeconds", 5);
        bool ok = true;
        ok &= melange::mem::HookIAT("steam_api.dll", "SteamNetworking", reinterpret_cast<void*>(&hkSteamNetworking),
                               reinterpret_cast<void**>(&oSteamNetworking));
        ok &= melange::mem::HookIAT("steam_api.dll", "SteamMatchmaking", reinterpret_cast<void*>(&hkSteamMatchmaking),
                               reinterpret_cast<void**>(&oSteamMatchmaking));
        ok &= melange::mem::HookIAT("steam_api.dll", "SteamAPI_RegisterCallback", reinterpret_cast<void*>(&hkRegCb),
                               reinterpret_cast<void**>(&oRegCb));
        ok &= melange::mem::HookIAT("steam_api.dll", "SteamAPI_UnregisterCallback", reinterpret_cast<void*>(&hkUnregCb),
                               reinterpret_cast<void**>(&oUnregCb));
        ok &= melange::mem::HookIAT("steam_api.dll", "SteamAPI_RegisterCallResult", reinterpret_cast<void*>(&hkRegCr),
                               reinterpret_cast<void**>(&oRegCr));
        ok &= melange::mem::HookIAT("steam_api.dll", "SteamAPI_UnregisterCallResult", reinterpret_cast<void*>(&hkUnregCr),
                               reinterpret_cast<void**>(&oUnregCr));

        using namespace melange::steam;
        for (auto [id, size] : {std::pair{kLobbyEnter, 24}, {kLobbyDataUpdate, 16}, {kLobbyChatUpdate, 32},
                                {kLobbyKicked, 24}, {kP2PSessionRequest, 8}, {kP2PSessionConnectFail, 16},
                                {kSocketStatus, 24}, {kSteamServersDisconnected, 4}, {kGameLobbyJoinRequested, 16}})
            new Listener(id, size);  // lives for the whole process

        melange::events::Subscribe(melange::events::Event::Frame, [] {
            static ULONGLONG last = 0;
            ULONGLONG now = GetTickCount64();
            if (now - last >= static_cast<ULONGLONG>(g_summarySeconds) * 1000) {
                last = now;
                LogSummary();
            }
        });
        for (auto e : {melange::events::Event::MatchStart, melange::events::Event::MatchEnd, melange::events::Event::LobbyEnter,
                       melange::events::Event::LobbyLeave})
            melange::events::Subscribe(e, [] {
                { std::lock_guard lk(g_lock); g_countersDirty = true; }
                LogSummary();
            });
        return ok;
    }
};
}  // namespace

MELANGE_MODULE(SteamTrace);
