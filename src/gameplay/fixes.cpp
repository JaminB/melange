// Fixes: guards for crashes in the unmodified game, one ini switch each.
#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <initializer_list>
#include <vector>

#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "gameplay/crashfix.h"

namespace {
using melange::crashfix::NullToSink;
using melange::crashfix::SchemeToStandIn;
using melange::crashfix::SepiaToStandIn;
using melange::crashfix::Sink;

std::vector<SafetyHookMid> g_hooks;

// At exit the service list destroys InputTranslationService before NetService, whose destructor then sets a flag on
// the dead instance: mov eax,[InputTranslationService]; or byte [eax+75h],2.
constexpr uintptr_t kNetDtor = 0x705FB4, kNetDtorOr = 0x705FB9;
Sink g_netSink{};
std::atomic<bool> g_netLogged{false};

void OnNetServiceDtor(safetyhook::Context& c) {
    if (NullToSink(c.eax, g_netSink) && !g_netLogged.exchange(true))
        LOG_INFO("[fixes] NetServiceExit: input service already destroyed; skipped its flag");
}

// The same exit releases AIService after g_Config, and its scene-graph reset tests g_Config's net-debug flag:
// mov eax,[g_Config]; test byte [eax+9Ah],2.
constexpr uintptr_t kAiReset = 0x4B20A0, kAiResetTest = 0x4B20A5;
Sink g_aiSink{};
std::atomic<bool> g_aiLogged{false};

void OnAiSceneReset(safetyhook::Context& c) {
    if (NullToSink(c.eax, g_aiSink) && !g_aiLogged.exchange(true))
        LOG_INFO("[fixes] AiServiceExit: config already destroyed; skipped the debug log");
}

// The /SEPIA switch is parsed before AppDataService exists and calls its post-process through a null pointer:
// mov eax,[AppDataService]; mov ecx,[eax+5Ch]; mov edx,[ecx]; mov eax,[edx+14h]; push 1; call eax.
// The stand-in records the call; it is replayed on the renderer's post-process once that exists.
constexpr uintptr_t kSepia = 0x4DAF9F, kSepiaLoad = 0x4DAFA4, kPostProcess = 0x961D7C;

void OnSepiaSwitch(safetyhook::Context& c) {
    if (SepiaToStandIn(c.eax)) LOG_INFO("[fixes] SepiaSwitch: /SEPIA parsed before the renderer exists; deferred");
}

// Composite takes the sepia colour and weight from tweaks looked up when the post-process is built; while either is
// missing it tints with weight 0. Resolvers for vector and float tweaks:
constexpr uintptr_t kResolveVector = 0x47B4C0, kResolveFloat = 0x465290;
bool g_resolvers = false;
uint32_t g_tintLogged = 0;

void ApplyPendingSepia() {
    using melange::crashfix::ResolveTweak;
    using melange::crashfix::Tint;
    if (!melange::crashfix::SepiaRequested()) return;
    uint32_t pp = 0;
    if (!melange::mem::SafeRead(kPostProcess, &pp, sizeof(pp)) || !pp) return;
    if (melange::crashfix::ApplySepia(pp)) LOG_INFO("[fixes] SepiaSwitch: sepia on (post-process %08x)", pp);
    const Tint t = melange::crashfix::EnsureSepiaTint(
        pp, g_resolvers ? reinterpret_cast<ResolveTweak>(kResolveVector) : nullptr,
        g_resolvers ? reinterpret_cast<ResolveTweak>(kResolveFloat) : nullptr);
    if (t != Tint::Present && g_tintLogged != pp) {
        g_tintLogged = pp;
        LOG_INFO("[fixes] SepiaSwitch: tint %s", t == Tint::Resolved ? "tweaks resolved" : "tweaks missing; shipped values");
    }
}

// A joining peer names the host's game style from the lobby's "scheme_code", an index into its own scheme list:
// call GetSchemes (count to [esp+8]); cmp edi,[esp+8]; jle; <assert>; mov eax,[esi+edi*4-4]; mov esi,[eax+14h].
// A host with mod styles the peer lacks sends an index past the end, and the assert only logs.
constexpr uintptr_t kSchemeCheck = 0x62680D, kSchemeLoad = 0x626829;
std::atomic<uint32_t> g_schemeLogged{0};

void OnSchemeName(safetyhook::Context& c) {
    const uint32_t count = *reinterpret_cast<const uint32_t*>(c.esp + 8);
    const uint32_t code = static_cast<uint32_t>(c.edi);
    if (SchemeToStandIn(c.esi, c.edi, count) && g_schemeLogged.exchange(code) != code)
        LOG_WARN("[fixes] SchemeCode: the host's game style #%d is not among this game's %u (a mod's?); shown as \"%s\"",
                 static_cast<int>(code), count, melange::crashfix::kUnknownScheme);
}

bool Guard(const char* name, uintptr_t check, std::initializer_list<int> bytes, uintptr_t site,
           safetyhook::MidHookFn fn) {
    if (!melange::mem::Expect(check, bytes)) {
        LOG_WARN("[fixes] %s: unexpected code at %08x; not installed", name, static_cast<unsigned>(check));
        return false;
    }
    auto h = safetyhook::create_mid(site, fn);
    if (!h) {
        LOG_ERROR("[fixes] %s: hook at %08x failed", name, static_cast<unsigned>(site));
        return false;
    }
    g_hooks.push_back(std::move(h));
    LOG_INFO("[fixes] %s: on", name);
    return true;
}

class Fixes final : public melange::Module {
public:
    const char* Name() const override { return "Fixes"; }
    const char* Description() const override { return "guards for crashes in the unmodified game"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 30; }

    bool Install() override {
        if (Bool("NetServiceExit", true))
            Guard("NetServiceExit", kNetDtor, {0xA1, 0xA4, 0xB2, 0x95, 0x00, 0x80, 0x48, 0x75, 0x02, 0x8D, 0x45, 0xFC},
                  kNetDtorOr, &OnNetServiceDtor);
        if (Bool("AiServiceExit", true))
            Guard("AiServiceExit", kAiReset, {0xA1, 0x00, 0xA1, 0x95, 0x00, 0xF6, 0x80, 0x9A, 0x00, 0x00, 0x00, 0x02},
                  kAiResetTest, &OnAiSceneReset);
        if (Bool("SepiaSwitch", true) &&
            Guard("SepiaSwitch", kSepia,
                  {0xA1, 0xE8, 0xA0, 0x95, 0x00, 0x8B, 0x48, 0x5C, 0x8B, 0x11, 0x8B, 0x42, 0x14, 0x6A, 0x01, 0xFF, 0xD0},
                  kSepiaLoad, &OnSepiaSwitch)) {
            g_resolvers = melange::mem::Expect(kResolveVector, {0x6A, 0xFF, 0x68, 0x38, 0x93, 0x7C, 0x00}) &&
                          melange::mem::Expect(kResolveFloat, {0x6A, 0xFF, 0x68, 0x38, 0x93, 0x7C, 0x00});
            melange::events::Subscribe(melange::events::Event::Frame, &ApplyPendingSepia);
        }
        if (Bool("SchemeCode", true))
            Guard("SchemeCode", kSchemeCheck,
                  {0x3B, 0x7C, 0x24, 0x08, 0x8B, 0xF0, 0x7E, 0x14, 0x68, 0x10, 0xE0, 0x86, 0x00, 0x68, 0xE8, 0x01, 0x00,
                   0x00, 0x68, 0x5C, 0xDD, 0x86, 0x00, 0xE8, 0x60, 0x20, 0x01, 0x00, 0x8B, 0x44, 0xBE, 0xFC, 0x8B, 0x70,
                   0x14},
                  kSchemeLoad, &OnSchemeName);
        return true;
    }

    void Uninstall() override { g_hooks.clear(); }
};
}  // namespace

MELANGE_MODULE(Fixes);
