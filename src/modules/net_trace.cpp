// NetTrace: logs the game's raw Winsock usage (WSOCK32 ordinal imports + WS2_32 overlapped I/O).
// Steam P2P carries the game traffic; this exists to catch anything else (legacy GameSpy/HTTP/XomOnline code).
#include <winsock2.h>
#include <windows.h>

#include <cstdio>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"

namespace {
#define CALLER wf::game::DescribeAddress(reinterpret_cast<uintptr_t>(_ReturnAddress())).c_str()

using socket_t = SOCKET(WINAPI*)(int, int, int);
using bind_t = int(WINAPI*)(SOCKET, const sockaddr*, int);
using connect_t = int(WINAPI*)(SOCKET, const sockaddr*, int);
using close_t = int(WINAPI*)(SOCKET);
using sendto_t = int(WINAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using gethost_t = hostent*(WINAPI*)(const char*);
socket_t oSocket;
bind_t oBind;
connect_t oConnect;
close_t oClose;
sendto_t oSendto;
gethost_t oGetHost;

const char* Addr(const sockaddr* a, char* buf, size_t n) {
    if (a && a->sa_family == AF_INET) {
        auto in = reinterpret_cast<const sockaddr_in*>(a);
        const auto* b = reinterpret_cast<const unsigned char*>(&in->sin_addr);
        snprintf(buf, n, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(in->sin_port));
    } else {
        snprintf(buf, n, "(family %d)", a ? a->sa_family : -1);
    }
    return buf;
}

SOCKET WINAPI hkSocket(int af, int type, int proto) {
    SOCKET s = oSocket(af, type, proto);
    WF_INFO("socket(af=%d type=%d proto=%d) = %u  [%s]", af, type, proto, static_cast<unsigned>(s), CALLER);
    return s;
}
int WINAPI hkBind(SOCKET s, const sockaddr* a, int n) {
    int r = oBind(s, a, n);
    char b[64];
    WF_INFO("bind(%u, %s) = %d  [%s]", static_cast<unsigned>(s), Addr(a, b, sizeof(b)), r, CALLER);
    return r;
}
int WINAPI hkConnect(SOCKET s, const sockaddr* a, int n) {
    int r = oConnect(s, a, n);
    char b[64];
    WF_INFO("connect(%u, %s) = %d  [%s]", static_cast<unsigned>(s), Addr(a, b, sizeof(b)), r, CALLER);
    return r;
}
int WINAPI hkClose(SOCKET s) {
    WF_INFO("closesocket(%u)  [%s]", static_cast<unsigned>(s), CALLER);
    return oClose(s);
}
int WINAPI hkSendto(SOCKET s, const char* d, int len, int flags, const sockaddr* a, int n) {
    static int logged = 0;
    if (logged < 50) {
        ++logged;
        char b[64];
        WF_INFO("sendto(%u, %d bytes, %s)  [%s]", static_cast<unsigned>(s), len, Addr(a, b, sizeof(b)), CALLER);
    }
    return oSendto(s, d, len, flags, a, n);
}
hostent* WINAPI hkGetHost(const char* name) {
    WF_INFO("gethostbyname(\"%s\")  [%s]", name ? name : "", CALLER);
    return oGetHost(name);
}

class NetTrace final : public wf::Module {
public:
    const char* Name() const override { return "NetTrace"; }
    const char* Description() const override { return "logs raw Winsock socket/bind/connect/sendto/DNS calls"; }
    int Order() const override { return 11; }

    bool Install() override {
        // WSOCK32 is imported by ordinal: 23 socket, 2 bind, 4 connect, 3 closesocket, 20 sendto, 52 gethostbyname
        wf::mem::HookIAT("WSOCK32.dll", "#23", reinterpret_cast<void*>(&hkSocket), reinterpret_cast<void**>(&oSocket));
        wf::mem::HookIAT("WSOCK32.dll", "#2", reinterpret_cast<void*>(&hkBind), reinterpret_cast<void**>(&oBind));
        wf::mem::HookIAT("WSOCK32.dll", "#4", reinterpret_cast<void*>(&hkConnect), reinterpret_cast<void**>(&oConnect));
        wf::mem::HookIAT("WSOCK32.dll", "#3", reinterpret_cast<void*>(&hkClose), reinterpret_cast<void**>(&oClose));
        wf::mem::HookIAT("WSOCK32.dll", "#20", reinterpret_cast<void*>(&hkSendto), reinterpret_cast<void**>(&oSendto));
        wf::mem::HookIAT("WSOCK32.dll", "#52", reinterpret_cast<void*>(&hkGetHost), reinterpret_cast<void**>(&oGetHost));
        return oSocket != nullptr;
    }
};
}  // namespace

WUMFIX_MODULE(NetTrace);
