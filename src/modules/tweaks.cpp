// Small quality-of-life tweaks. Also serves as the reference example for writing a module:
//  - WindowTag shows a pure-API module that works on any exe build (per-frame event subscriber).
//  - FrameInterval shows a hard-coded-address patch: RequiresKnownBuild() + verify original bytes first.
#include <windows.h>

#include <cstdio>

#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "version.h"

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

namespace {
class WindowTag final : public wf::Module {
public:
    const char* Name() const override { return "WindowTag"; }
    const char* Description() const override { return "appends [WUMFix x.y.z] to the game window title"; }

    bool Install() override {
        wf::events::Subscribe(wf::events::Event::Frame, [] {
            if (wf::events::FrameCount() % 120 != 1) return;  // cheap: check every ~2 s
            HWND w = static_cast<HWND>(wf::events::GameWindow());
            if (!w) return;
            wchar_t title[256];
            GetWindowTextW(w, title, 256);
            if (wcsstr(title, L"[WUMFix")) return;
            wchar_t tagged[320];
            swprintf(tagged, 320, L"%s [WUMFix " WIDEN(WUMFIX_VERSION) L"]", title);
            SetWindowTextW(w, tagged);
        });
        return true;
    }
};

class FrameInterval final : public wf::Module {
public:
    const char* Name() const override { return "FrameInterval"; }
    const char* Description() const override { return "engine frame interval in ms (16 = ~60 fps, 8 = ~120 fps)"; }
    bool DefaultEnabled() const override { return false; }
    bool RequiresKnownBuild() const override { return true; }

    bool Install() override {
        int ms = Int("IntervalMs", 16);
        if (ms < 1 || ms > 100) return false;
        // 004D919A: cmp eax, 10h / jnb ... / mov ecx, 10h   (same site WUMPatch uses)
        constexpr uintptr_t kCmp = 0x4D919B, kMov = 0x4D919F;
        if (!wf::mem::Expect(kCmp, {0x10, 0x73}) || !wf::mem::Expect(kMov, {0x10, 0x00, 0x00, 0x00})) return false;
        wf::mem::Put<uint8_t>(kCmp, static_cast<uint8_t>(ms));
        wf::mem::Put<uint8_t>(kMov, static_cast<uint8_t>(ms));
        WF_INFO("FrameInterval: set to %d ms", ms);
        return true;
    }
};
}  // namespace

WUMFIX_MODULE(WindowTag);
WUMFIX_MODULE(FrameInterval);
