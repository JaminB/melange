#pragma once
// Typed access to the game's network objects for build #1077.
// All reads are SEH-guarded: a bad address yields a default value instead of a crash.
#include <cstdint>

namespace melange::wum {
namespace addr {
constexpr uintptr_t NetServicePtr = 0x979ddc;        // NetService* singleton
constexpr uintptr_t AbortGame = 0x70864c;            // __thiscall(ns, HRESULT), ret 4
constexpr uintptr_t SurrenderPlayer = 0x708a97;      // __thiscall(ns, player), ret 4
constexpr uintptr_t TurnStarted = 0x709827;          // __thiscall(ns, event msg); looks the player up itself
constexpr uintptr_t CheckViability = 0x4190ac;       // __thiscall(session, nOffset), ret 4
constexpr uintptr_t PlayerCount = 0x67cb62;          // __thiscall(container) -> int
constexpr uintptr_t PlayerAt = 0x67c982;             // __thiscall(container, idx) -> NetPlayer*, ret 4
constexpr uintptr_t CurrentPlayer = 0x706432;        // __thiscall(ns) -> NetPlayer* of CurrentTeamIndex, or 0
constexpr uintptr_t SteamConnCtor = 0x78613e;        // XSteamConnection::XSteamConnection (ecx = this)
constexpr uintptr_t SteamConnDtor = 0x785c85;        // XSteamConnection::~XSteamConnection
constexpr uintptr_t SteamConnNewSender = 0x785f44;   // listener: packet from an unknown sender -> new connection
constexpr uintptr_t DeadChannelBranch = 0x70a7e5;    // ja 0x70abf6 (surrender path) in NetService::Update
constexpr uintptr_t ThrottleSetPaused = 0x7059f2;    // __thiscall NetThrottle::SetPaused(bool), ret 4
constexpr uintptr_t Unpause = 0x4d76f0;              // void __cdecl AppDataService Unpause()
constexpr uintptr_t AppPtr = 0x96d1cc;               // XomApp* (XomGetApp); +0x1c timescale, +0x70 pause refcount
constexpr uintptr_t TaskManagerPtr = 0x96d030;       // TM*; +0x38 sim clock, +0x3c paused
constexpr uintptr_t ConfigPtr = 0x95a100;            // g_Config*; +0x9a bit 2 = verbose net logging (/LOG ALL)
}  // namespace addr

// NetService state machine: ns+0x20 holds the current state function.
namespace state {
constexpr uintptr_t Init = 0x70bda0;
constexpr uintptr_t WaitingSimChannel = 0x70b86e;
constexpr uintptr_t WaitingGameStart = 0x70b334;  // in lobby, waiting for host to start
constexpr uintptr_t WaitingConnections = 0x70ad51;
constexpr uintptr_t WaitingLoad = 0x709f2f;
constexpr uintptr_t InGame = 0x709e5f;
constexpr uintptr_t ProcessWinOrDraw = 0x70b740;
constexpr uintptr_t WaitingUnload = 0x70b557;
}  // namespace state

namespace off {
// NetService
constexpr uintptr_t State = 0x20;
constexpr uintptr_t ViabilityArmed = 0x89;
constexpr uintptr_t Session = 0x8c;
constexpr uintptr_t Players = 0x4a8;  // pointer to the player container
constexpr uintptr_t BeginGameDone = 0x4bc;
constexpr uintptr_t InGameFlag = 0x4bd;
constexpr uintptr_t GameEnded = 0x4be;
constexpr uintptr_t CurrentSurrendered = 0x4c0;
constexpr uintptr_t ValidationFifoCount = 0x4dc;
constexpr uintptr_t Throttle = 0x4d8;  // NetThrottle*
// NetThrottle
constexpr uintptr_t ThrottlePaused = 0x20;
constexpr uintptr_t ThrottleAuto = 0x21;
constexpr uintptr_t ThrottleMask = 0x22;  // 0x3f = all six NetStored types received
constexpr uintptr_t ThrottleLead = 0x24;
// XomOnlineSession
constexpr uintptr_t SessionStateBits = 0x18;
constexpr uintptr_t SessionPlayerCount = 0x4c;
constexpr uintptr_t SessionNOffset = 0x80;
// NetPlayer
constexpr uintptr_t PlayerChannel = 0x40;
constexpr uintptr_t PlayerIsLocal = 0x4e;
constexpr uintptr_t PlayerLoaded = 0x51;
constexpr uintptr_t PlayerSurrenderNext = 0x52;
constexpr uintptr_t PlayerSurrender2 = 0x53;
// XSteamConnection
constexpr uintptr_t ConnSendSeq = 0x24;
constexpr uintptr_t ConnRecvSeq = 0x28;
constexpr uintptr_t ConnState = 0x30;
}  // namespace off

uintptr_t NetService();  // 0 if not created yet
uintptr_t CurrentState();
const char* StateName(uintptr_t fn);
int PlayerCount(uintptr_t ns);
uintptr_t PlayerAt(uintptr_t ns, int idx);
uintptr_t CurrentPlayer(uintptr_t ns);  // NetPlayer whose turn it is (0 between turns)

template <class T>
T Read(uintptr_t addr, T def = T{});
bool WriteByte(uintptr_t addr, uint8_t v);
bool WriteInt(uintptr_t addr, int32_t v);
}  // namespace melange::wum
