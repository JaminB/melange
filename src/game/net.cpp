#include "game/net.h"

#include <windows.h>

#include "core/mem.h"

namespace wf::wum {
template <class T>
T Read(uintptr_t addr, T def) {
    T v;
    return addr && mem::SafeRead(addr, &v, sizeof(T)) ? v : def;
}
template uint8_t Read<uint8_t>(uintptr_t, uint8_t);
template int32_t Read<int32_t>(uintptr_t, int32_t);
template uint32_t Read<uint32_t>(uintptr_t, uint32_t);

bool WriteByte(uintptr_t addr, uint8_t v) {
    __try {
        *reinterpret_cast<volatile uint8_t*>(addr) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteInt(uintptr_t addr, int32_t v) {
    __try {
        *reinterpret_cast<volatile int32_t*>(addr) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t NetService() { return Read<uint32_t>(addr::NetServicePtr); }

uintptr_t CurrentState() {
    uintptr_t ns = NetService();
    return ns ? Read<uint32_t>(ns + off::State) : 0;
}

const char* StateName(uintptr_t fn) {
    switch (fn) {
        case 0: return "none";
        case state::Init: return "Init";
        case state::WaitingSimChannel: return "WaitingSimChannel";
        case state::WaitingGameStart: return "WaitingGameStart(lobby)";
        case state::WaitingConnections: return "WaitingConnections";
        case state::WaitingLoad: return "WaitingLoad";
        case state::InGame: return "InGame";
        case state::ProcessWinOrDraw: return "ProcessWinOrDraw";
        case state::WaitingUnload: return "WaitingUnload";
        default: return "?";
    }
}

namespace {
using Count_t = int(__thiscall*)(uintptr_t);
using At_t = uintptr_t(__thiscall*)(uintptr_t, int);

int SafeCount(uintptr_t c) {
    __try {
        return reinterpret_cast<Count_t>(addr::PlayerCount)(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uintptr_t SafeAt(uintptr_t c, int i) {
    __try {
        return reinterpret_cast<At_t>(addr::PlayerAt)(c, i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
}  // namespace

// ns+0x4a8 holds a POINTER to the player container (0x706432: mov ecx,[ebx+0x4a8]; call 0x67cb62).
uintptr_t PlayerContainer(uintptr_t ns) { return ns ? Read<uint32_t>(ns + off::Players) : 0; }
int PlayerCount(uintptr_t ns) {
    uintptr_t c = PlayerContainer(ns);
    return c ? SafeCount(c) : 0;
}
uintptr_t PlayerAt(uintptr_t ns, int idx) {
    uintptr_t c = PlayerContainer(ns);
    return c ? SafeAt(c, idx) : 0;
}
uintptr_t CurrentPlayer(uintptr_t ns) {
    if (!ns) return 0;
    __try {
        return reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t)>(addr::CurrentPlayer)(ns);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
}  // namespace wf::wum
