#pragma once
// Minimal Steamworks declarations matching the SDK the game shipped with
// (steam_api.dll 01.10.01.46: SteamNetworking005, SteamMatchMaking008, SteamUser016).
// We do not ship the Steamworks SDK; only what WUMFix needs is declared here.
#include <windows.h>

#include <cstdint>

namespace wf::steam {
using SteamID = uint64_t;
using SteamAPICall = uint64_t;

// Must match the SDK's CCallbackBase exactly: we compile with MSVC like the game, so vtable layout matches.
class CallbackBase {
public:
    CallbackBase() = default;
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, SteamAPICall call) = 0;
    virtual int GetCallbackSizeBytes() = 0;

protected:
    enum { kFlagRegistered = 0x01, kFlagGameServer = 0x02 };
    uint8_t callbackFlags_ = 0;
    int callbackId_ = 0;
    friend void RegisterCallback(CallbackBase*, int);
};

// Callback ids (k_iCallback) used by the game's netcode.
enum CallbackId : int {
    kSteamServersConnected = 101,
    kSteamServerConnectFailure = 102,
    kSteamServersDisconnected = 103,
    kGameLobbyJoinRequested = 333,
    kLobbyEnter = 504,
    kLobbyDataUpdate = 505,
    kLobbyChatUpdate = 506,
    kLobbyChatMsg = 507,
    kLobbyGameCreated = 509,
    kLobbyMatchList = 510,
    kLobbyKicked = 512,
    kLobbyCreated = 513,
    kSocketStatus = 1201,
    kP2PSessionRequest = 1202,
    kP2PSessionConnectFail = 1203,
};

#pragma pack(push, 8)
struct P2PSessionState {
    uint8_t connectionActive;
    uint8_t connecting;
    uint8_t p2pSessionError;
    uint8_t usingRelay;
    int32_t bytesQueuedForSend;
    int32_t packetsQueuedForSend;
    uint32_t remoteIP;
    uint16_t remotePort;
};
#pragma pack(pop)

// Resolved lazily from the already-loaded steam_api.dll.
void RegisterCallback(CallbackBase* cb, int id);
void UnregisterCallback(CallbackBase* cb);
const char* CallbackName(int id);
}  // namespace wf::steam
