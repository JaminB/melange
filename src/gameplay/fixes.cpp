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

void ApplyPendingSepia() {
    if (!melange::crashfix::SepiaRequested()) return;
    uint32_t pp = 0;
    if (!melange::mem::SafeRead(kPostProcess, &pp, sizeof(pp))) return;
    if (melange::crashfix::ApplySepia(pp)) LOG_INFO("[fixes] SepiaSwitch: sepia on (post-process %08x)", pp);
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
                  kSepiaLoad, &OnSepiaSwitch))
            melange::events::Subscribe(melange::events::Event::Frame, &ApplyPendingSepia);
        return true;
    }

    void Uninstall() override { g_hooks.clear(); }
};
}  // namespace

MELANGE_MODULE(Fixes);
