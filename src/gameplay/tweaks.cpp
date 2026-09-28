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
class WindowTag final : public melange::Module {
public:
    const char* Name() const override { return "WindowTag"; }
    const char* Description() const override { return "appends [Melange x.y.z] to the game window title"; }
    // Pinned explicitly (was the implicit default 100, tied with FrameInterval): the repo restructure
    // changes translation-unit link order, so the two can no longer rely on that to install in the
    // order this file registers them. These values keep today's order (WindowTag, then FrameInterval).
    int Order() const override { return 100; }

    bool Install() override {
        melange::events::Subscribe(melange::events::Event::Frame, [] {
            if (melange::events::FrameCount() % 120 != 1) return;  // cheap: check every ~2 s
            HWND w = static_cast<HWND>(melange::events::GameWindow());
            if (!w) return;
            wchar_t title[256];
            GetWindowTextW(w, title, 256);
            if (wcsstr(title, L"[Melange")) return;
            wchar_t tagged[320];
            swprintf(tagged, 320, L"%s [Melange " WIDEN(MELANGE_VERSION) L"]", title);
            SetWindowTextW(w, tagged);
        });
        return true;
    }
};

class FrameInterval final : public melange::Module {
public:
    const char* Name() const override { return "FrameInterval"; }
    const char* Description() const override { return "engine frame interval in ms (16 = ~60 fps, 8 = ~120 fps)"; }
    bool DefaultEnabled() const override { return false; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 101; }  // see WindowTag::Order()

    bool Install() override {
        int ms = Int("IntervalMs", 16);
        if (ms < 1 || ms > 100) return false;
        // 004D919A: cmp eax, 10h / jnb ... / mov ecx, 10h   (same site WUMPatch uses)
        constexpr uintptr_t kCmp = 0x4D919B, kMov = 0x4D919F;
        if (!melange::mem::Expect(kCmp, {0x10, 0x73}) || !melange::mem::Expect(kMov, {0x10, 0x00, 0x00, 0x00})) return false;
        melange::mem::Put<uint8_t>(kCmp, static_cast<uint8_t>(ms));
        melange::mem::Put<uint8_t>(kMov, static_cast<uint8_t>(ms));
        WF_INFO("FrameInterval: set to %d ms", ms);
        return true;
    }
};
}  // namespace

MELANGE_MODULE(WindowTag);
MELANGE_MODULE(FrameInterval);
