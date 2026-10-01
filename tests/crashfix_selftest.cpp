// Offline self-test for src/gameplay/crashfix.*: each guard runs as a SafetyHook mid hook on a copy of the game's
// faulting instruction sequence, with the singleton pointer null (redirected to the sink) and set (left alone).
#include <windows.h>

#include <safetyhook.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

#include "gameplay/crashfix.h"

using namespace melange::crashfix;

namespace {
int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

uint8_t* g_code = nullptr;
uintptr_t g_slot = 0;
Sink g_sink{};

// Copies `bytes` to executable memory; 0xA1 at offset 0 gets the address of g_slot as its operand.
uintptr_t Emit(std::vector<uint8_t> bytes) {
    static size_t used = 0;
    const uintptr_t slot = reinterpret_cast<uintptr_t>(&g_slot);
    if (bytes[0] == 0xA1) std::memcpy(&bytes[1], &slot, 4);
    uint8_t* at = g_code + used;
    std::memcpy(at, bytes.data(), bytes.size());
    used += (bytes.size() + 15) & ~size_t{15};
    return reinterpret_cast<uintptr_t>(at);
}

void OnSite(safetyhook::Context& c) { NullToSink(c.eax, g_sink); }

void TestNullToSink() {
    uintptr_t reg = 0;
    Check(NullToSink(reg, g_sink) && reg == reinterpret_cast<uintptr_t>(g_sink.bytes), "null base goes to the sink");
    reg = 0x1234;
    Check(!NullToSink(reg, g_sink) && reg == 0x1234, "live base is left alone");
}

// NetService destructor, 0x705FB4: mov eax,[slot]; or byte [eax+75h],2; lea eax,[ebp-4]; ret
void TestNetServiceExit() {
    const uintptr_t fn = Emit({0xA1, 0, 0, 0, 0, 0x80, 0x48, 0x75, 0x02, 0x8D, 0x45, 0xFC, 0xC3});
    auto hook = safetyhook::create_mid(fn + 5, &OnSite);
    Check(static_cast<bool>(hook), "NetServiceExit: hook installs on the game's bytes");
    if (!hook) return;
    std::memset(g_sink.bytes, 0, sizeof(g_sink.bytes));
    g_slot = 0;
    reinterpret_cast<void (*)()>(fn)();
    Check(g_sink.bytes[0x75] == 2, "NetServiceExit: destroyed service -> flag lands in the sink");
    alignas(16) uint8_t live[0x100] = {};
    g_slot = reinterpret_cast<uintptr_t>(live);
    g_sink.bytes[0x75] = 0;
    reinterpret_cast<void (*)()>(fn)();
    Check(live[0x75] == 2 && g_sink.bytes[0x75] == 0, "NetServiceExit: live service -> flag set as before");
}
}  // namespace

int main() {
    g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_code) return 2;
    TestNullToSink();
    TestNetServiceExit();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
