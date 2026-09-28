#include <windows.h>

#include <cstring>
#include <mutex>

#include "core/log.h"
#include "render/mirage/hub.h"
#include "render/mirage/trace_internal.h"

namespace melange::mirage::trace {
namespace {
constexpr int kSlots = 256, kStubBytes = 16;
struct Slot {
    TapFn fn;
    void* user;
    void* next;
};
Slot g_slot[kSlots];
int g_n = 0;
uint8_t* g_stubs = nullptr;
std::mutex g_mx;

void* __cdecl TapCall(uint32_t slot, const uint32_t* f) {
    Slot& s = g_slot[slot];
    s.fn(static_cast<int>(slot), f, s.user);
    return s.next;
}

// Entered from a stub with the slot pushed on top of the caller's return address; leaves the stack exactly as the
// caller built it and continues at the slot's `next` (same trick as the hub's log stub).
__declspec(naked) void TapStub() {
    __asm {
        pushad
        pushfd
        lea eax, [esp + 40]
        push eax
        push dword ptr [esp + 40]
        call TapCall
        add esp, 8
        mov [esp + 36], eax
        popfd
        popad
        ret
    }
}
}  // namespace

int AddTap(const char* glName, TapFn fn, void* user) {
    std::lock_guard lk(g_mx);
    if (!fn || !hub::Installed() || g_n >= kSlots) return -1;
    if (!g_stubs) {
        g_stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSlots * kStubBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_stubs) return -1;
    }
    int i = g_n;
    uint8_t* t = g_stubs + i * kStubBytes;
    // push imm32 ; jmp rel32 TapStub
    t[0] = 0x68;
    uint32_t v = static_cast<uint32_t>(i);
    memcpy(t + 1, &v, 4);
    t[5] = 0xE9;
    v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&TapStub) - reinterpret_cast<uintptr_t>(t + 10));
    memcpy(t + 6, &v, 4);
    FlushInstructionCache(GetCurrentProcess(), t, kStubBytes);
    g_slot[i] = {fn, user, nullptr};
    // `next` may stay null until the game resolves the function; the hub fills it before the stub can run
    if (!hub::Interpose(glName, t, &g_slot[i].next)) {
        LOG_WARN("[mirage] trace: cannot tap %s", glName);
        return -1;
    }
    g_n = i + 1;
    return i;
}
}  // namespace melange::mirage::trace
