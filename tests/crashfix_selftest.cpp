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
// A tweak resource double laid out like the game's: the value at details+0x1C.
struct FakeTweak {
    void** vtbl;
    float* details;
};
float g_realDetails[10] = {0, 0, 0, 0, 0, 0, 0, 0.25f, 0.5f, 0.75f};
FakeTweak g_realTweak{g_fakeVtbl, g_realDetails};
int g_resolves = 0;

int __cdecl ResolveFound(const char* const* name, uint32_t* out) {
    ++g_resolves;
    if (!name || !*name || std::strncmp(*name, "Sepia.", 6) != 0) return -1;
    *out = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_realTweak));
    return 0;
}

int __cdecl ResolveMissing(const char* const*, uint32_t*) {
    ++g_resolves;
    return -1;
}

uint32_t Slot(const uint8_t* pp, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, pp + off, sizeof(v));
    return v;
}

// What Composite reads: (*(slot))->details + 0x1C.
const float* TweakValue(uint32_t holder) {
    const float* details = nullptr;
    std::memcpy(&details, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(holder)) + 4, sizeof(details));
    return details + 7;
}

void TestSepiaTint() {
    alignas(16) uint8_t pp[0x84] = {};
    g_resolves = 0;
    Check(EnsureSepiaTint(reinterpret_cast<uintptr_t>(pp), &ResolveFound, &ResolveFound) == Tint::Resolved &&
              Slot(pp, 0x34) == reinterpret_cast<uintptr_t>(&g_realTweak) &&
              Slot(pp, 0x38) == reinterpret_cast<uintptr_t>(&g_realTweak) && g_resolves == 2,
          "SepiaTint: missing tweaks are looked up by name");
    Check(EnsureSepiaTint(reinterpret_cast<uintptr_t>(pp), &ResolveFound, &ResolveFound) == Tint::Present &&
              g_resolves == 2,
          "SepiaTint: present tweaks are left alone (no lookup)");

    std::memset(pp, 0, sizeof(pp));
    Check(EnsureSepiaTint(reinterpret_cast<uintptr_t>(pp), &ResolveMissing, nullptr) == Tint::Fallback,
          "SepiaTint: unresolvable tweaks get holders");
    const float* c = TweakValue(Slot(pp, 0x34));
    const float* w = TweakValue(Slot(pp, 0x38));
    Check(c[0] == 1.0f && c[1] == 0.8f && c[2] == 0.6f && *w == 0.4f, "SepiaTint: holders carry the shipped tweak values");
    using Release = uint32_t(__fastcall*)(void*, void*);
    void* holder = reinterpret_cast<void*>(static_cast<uintptr_t>(Slot(pp, 0x34)));
    reinterpret_cast<Release>((*static_cast<void***>(holder))[2])(holder, nullptr);
    Check(TweakValue(Slot(pp, 0x34))[0] == 1.0f, "SepiaTint: releasing a holder is harmless");

    alignas(16) uint8_t half[0x84] = {};
    const uint32_t real = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_realTweak));
    std::memcpy(half + 0x34, &real, sizeof(real));
    Check(EnsureSepiaTint(reinterpret_cast<uintptr_t>(half), nullptr, nullptr) == Tint::Fallback &&
              Slot(half, 0x34) == real && *TweakValue(Slot(half, 0x38)) == 0.4f,
          "SepiaTint: only the missing tweak is filled");
    Check(EnsureSepiaTint(0, &ResolveFound, &ResolveFound) == Tint::Present, "SepiaTint: no post-process, no-op");
}
}  // namespace

int main() {
    g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_code) return 2;
    TestNullToSink();
    TestNetServiceExit();
    TestAiServiceExit();
    TestSepiaSwitch();
    TestSepiaTint();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "all passed", g_failures);
    return g_failures ? 1 : 0;
}
