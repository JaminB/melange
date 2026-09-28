#include "net/steam.h"

#include "core/log.h"
#include "core/mem.h"

namespace melange::steam {
namespace {
using Register_t = void(__cdecl*)(CallbackBase*, int);
using Unregister_t = void(__cdecl*)(CallbackBase*);

template <class T>
T Resolve(const char* name) {
    HMODULE api = GetModuleHandleW(L"steam_api.dll");
    return api ? reinterpret_cast<T>(GetProcAddress(api, name)) : nullptr;
}

// Through the game's own import slot when it has one, so whatever the game's Steam calls go through sees ours too.
template <class T>
T ResolveAsGame(const char* name) {
    void** slot = mem::FindIAT("steam_api.dll", name);
    return slot && *slot ? reinterpret_cast<T>(*slot) : Resolve<T>(name);
}
}  // namespace

void RegisterCallback(CallbackBase* cb, int id) {
    auto fn = ResolveAsGame<Register_t>("SteamAPI_RegisterCallback");
    if (!fn) {
        LOG_ERROR("steam: SteamAPI_RegisterCallback not found");
        return;
    }
    fn(cb, id);  // sets callbackId_ and the registered flag itself
}

void UnregisterCallback(CallbackBase* cb) {
    auto fn = ResolveAsGame<Unregister_t>("SteamAPI_UnregisterCallback");
    if (fn) fn(cb);
}

const char* CallbackName(int id) {
    switch (id) {
        case kSteamServersConnected: return "SteamServersConnected";
        case kSteamServerConnectFailure: return "SteamServerConnectFailure";
        case kSteamServersDisconnected: return "SteamServersDisconnected";
        case kGameLobbyJoinRequested: return "GameLobbyJoinRequested";
        case kLobbyEnter: return "LobbyEnter";
        case kLobbyDataUpdate: return "LobbyDataUpdate";
        case kLobbyChatUpdate: return "LobbyChatUpdate";
        case kLobbyChatMsg: return "LobbyChatMsg";
        case kLobbyGameCreated: return "LobbyGameCreated";
        case kLobbyMatchList: return "LobbyMatchList";
        case kLobbyKicked: return "LobbyKicked";
        case kLobbyCreated: return "LobbyCreated";
        case kSocketStatus: return "SocketStatus";
        case kP2PSessionRequest: return "P2PSessionRequest";
        case kP2PSessionConnectFail: return "P2PSessionConnectFail";
        default: return "?";
    }
}
}  // namespace melange::steam
