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

// AISceneGraphService::Reset, 0x4B20A0: mov eax,[slot]; test byte [eax+9Ah],2; setnz al; movzx eax,al; ret
void TestAiServiceExit() {
    const uintptr_t fn = Emit({0xA1, 0, 0, 0, 0, 0xF6, 0x80, 0x9A, 0x00, 0x00, 0x00, 0x02, 0x0F, 0x95, 0xC0, 0x0F, 0xB6,
                               0xC0, 0xC3});
    auto hook = safetyhook::create_mid(fn + 5, &OnSite);
    Check(static_cast<bool>(hook), "AiServiceExit: hook installs on the game's bytes");
    if (!hook) return;
    auto flag = reinterpret_cast<int (*)()>(fn);
    std::memset(g_sink.bytes, 0, sizeof(g_sink.bytes));
    g_slot = 0;
    Check(flag() == 0, "AiServiceExit: destroyed config -> debug flag reads clear");
    alignas(16) uint8_t live[0x100] = {};
    live[0x9a] = 2;
    g_slot = reinterpret_cast<uintptr_t>(live);
    Check(flag() == 1, "AiServiceExit: live config -> debug flag read as before");
}

// A post-process double: slot 5 records `this` and the flag.
struct FakePostProcess {
    void** vtbl;
    void* lastThis = nullptr;
    int lastOn = -1;
    int calls = 0;
};

void __fastcall FakeSetSepia(void* self, void*, int on) {
    auto* pp = static_cast<FakePostProcess*>(self);
    pp->lastThis = self;
    pp->lastOn = on;
    ++pp->calls;
}

void* g_fakeVtbl[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, reinterpret_cast<void*>(&FakeSetSepia)};

void OnSepiaSite(safetyhook::Context& c) { SepiaToStandIn(c.eax); }

// /SEPIA switch, 0x4DAF9F: mov eax,[slot]; mov ecx,[eax+5Ch]; mov edx,[ecx]; mov eax,[edx+14h]; push 1; call eax; ret
void TestSepiaSwitch() {
    const uintptr_t fn = Emit({0xA1, 0, 0, 0, 0, 0x8B, 0x48, 0x5C, 0x8B, 0x11, 0x8B, 0x42, 0x14, 0x6A, 0x01, 0xFF, 0xD0,
                               0xC3});
    auto hook = safetyhook::create_mid(fn + 5, &OnSepiaSite);
    Check(static_cast<bool>(hook), "SepiaSwitch: hook installs on the game's bytes");
    if (!hook) return;
    auto parse = reinterpret_cast<void (*)()>(fn);

    ResetSepia();
    g_slot = 0;
    parse();
    Check(SepiaRequested(), "SepiaSwitch: no AppDataService -> request recorded (and the stack balanced)");
    FakePostProcess a{g_fakeVtbl}, b{g_fakeVtbl};
    Check(!ApplySepia(0), "SepiaSwitch: nothing applied before the post-process exists");
    Check(ApplySepia(reinterpret_cast<uintptr_t>(&a)) && a.calls == 1 && a.lastOn == 1 && a.lastThis == &a,
          "SepiaSwitch: SetSepia(1) replayed on the post-process");
    Check(!ApplySepia(reinterpret_cast<uintptr_t>(&a)) && a.calls == 1, "SepiaSwitch: replayed once per instance");
    Check(ApplySepia(reinterpret_cast<uintptr_t>(&b)) && b.calls == 1, "SepiaSwitch: a new post-process gets it too");

    alignas(16) uint8_t app[0x60] = {};
    ResetSepia();
    g_slot = reinterpret_cast<uintptr_t>(app);
    parse();
    Check(SepiaRequested(), "SepiaSwitch: AppDataService without a post-process -> request recorded");

    FakePostProcess live{g_fakeVtbl};
    const uint32_t livePtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&live));
    std::memcpy(app + 0x5C, &livePtr, sizeof(livePtr));
    ResetSepia();
    parse();
    Check(!SepiaRequested() && live.calls == 1 && live.lastOn == 1 && live.lastThis == &live,
          "SepiaSwitch: live post-process -> the game's own call runs");
}
}  // namespace

int main() {
    g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_code) return 2;
    TestNullToSink();
    TestNetServiceExit();
    TestAiServiceExit();
    TestSepiaSwitch();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
