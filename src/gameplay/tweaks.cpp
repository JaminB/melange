// Reference modules: WindowTag uses only the API (any build); FrameInterval patches fixed addresses.
#include <windows.h>

#include <timeapi.h>

#include <cstdio>

#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/overlay.h"
#include "version.h"

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

namespace {
class WindowTag final : public melange::Module {
public:
    const char* Name() const override { return "WindowTag"; }
    const char* Description() const override { return "appends [Melange x.y.z] to the game window title"; }
    int Order() const override { return 100; }

    bool Install() override {
        melange::events::Subscribe(melange::events::Event::Frame, [] {
            if (melange::events::FrameCount() % 120 != 1) return;
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
    const char* Description() const override {
        return "engine frame interval in ms (16 = ~60 fps, 8 = ~120 fps); classic timing (timeBeginPeriod)";
    }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 101; }

    bool Install() override {
        classicTiming_ = Bool("ClassicTiming", false);
        if (classicTiming_) ApplyClassicTiming(true);
        melange::overlay::AddMenuItem("Game/Classic timing (toggle)",
                                      [](void* self) { static_cast<FrameInterval*>(self)->ToggleClassicTiming(); },
                                      this);

        // 16 is the engine's own value: leave the site untouched so SmoothSixty can still lift it.
        int ms = Int("IntervalMs", 16);
        if (ms == 16) return true;
        if (ms < 1 || ms > 100) {
            LOG_WARN("FrameInterval: IntervalMs=%d out of range, not patched", ms);
            return true;
        }
        // 004D919A: cmp eax, 10h / jnb ... / mov ecx, 10h (the same site WUMPatch patches)
        constexpr uintptr_t kCmp = 0x4D919B, kMov = 0x4D919F;
        if (!melange::mem::Expect(kCmp, {0x10, 0x73}) || !melange::mem::Expect(kMov, {0x10, 0x00, 0x00, 0x00})) {
            LOG_WARN("FrameInterval: limiter site does not match the expected bytes; not patched");
            return true;
        }
        melange::mem::Put<uint8_t>(kCmp, static_cast<uint8_t>(ms));
        melange::mem::Put<uint8_t>(kMov, static_cast<uint8_t>(ms));
        LOG_INFO("FrameInterval: set to %d ms", ms);
        return true;
    }

    void Uninstall() override { ApplyClassicTiming(false); }

private:
    bool classicTiming_ = false;
    bool classicActive_ = false;

    // Raises (or restores) the OS timer resolution to 1 ms, for steadier frame pacing on systems where the
    // default ~15.6 ms resolution makes the game stutter. Calls are kept balanced so Toggle() and Uninstall()
    // never double up timeBeginPeriod/timeEndPeriod.
    void ApplyClassicTiming(bool on) {
        if (on == classicActive_) return;
        MMRESULT r = on ? timeBeginPeriod(1) : timeEndPeriod(1);
        LOG_INFO("FrameInterval: %s(1) -> %u", on ? "timeBeginPeriod" : "timeEndPeriod", r);
        classicActive_ = on;
    }

    void ToggleClassicTiming() {
        classicTiming_ = !classicTiming_;
        ApplyClassicTiming(classicTiming_);
        melange::config::SetString(Name(), "ClassicTiming", classicTiming_ ? "1" : "0");
        LOG_INFO("FrameInterval: classic timing %s", classicTiming_ ? "on" : "off");
    }
};

// Lifts the Sleep(16 - elapsed) limiter at FrameInterval's site and forces vsync. Leaves the site alone if another
// patch already changed it.
class SmoothSixty final : public melange::Module {
public:
    const char* Name() const override { return "SmoothSixty"; }
    const char* Description() const override {
        return "Smooth 60: lifts the engine's Sleep(16) frame limiter and forces vsync";
    }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 102; }

    bool Install() override {
        on_ = Bool("On", false);
        if (on_ && !Apply(true)) on_ = false;
        melange::events::Subscribe(melange::events::Event::Frame, [this] { EnsureVsync(); });
        melange::overlay::AddMenuItem("Game/Smooth 60 (toggle)",
                                      [](void* self) { static_cast<SmoothSixty*>(self)->Toggle(); }, this);
        LOG_INFO("SmoothSixty: ready (on=%d)", on_);
        return true;
    }

    void Uninstall() override {
        Apply(false);
        RestoreVsync();
    }

private:
    static constexpr uintptr_t kCmp = 0x4D919B, kMov = 0x4D919F;
    bool on_ = false;
    bool patched_ = false;
    uint8_t saved_ = 0x10;
    bool vsyncSaved_ = false;
    int savedInterval_ = 0;

    void Toggle() {
        bool want = !on_;
        bool ok = Apply(want);
        on_ = want && ok;
        melange::config::SetString(Name(), "On", on_ ? "1" : "0");
        if (want && !ok) {
            LOG_WARN("SmoothSixty: refused (limiter site already patched by another module); staying off");
        } else {
            LOG_INFO("SmoothSixty: %s", on_ ? "on" : "off");
            if (!on_) RestoreVsync();
        }
    }

    // Returns whether the limiter is now in the requested state.
    bool Apply(bool lift) {
        if (lift) {
            if (patched_) return true;
            uint8_t cur = 0x10;
            if (!melange::mem::SafeRead(kCmp, &cur, 1)) return false;
            if (cur != 0x10) {
                LOG_WARN("SmoothSixty: limiter byte is already 0x%02x (another module patched it); leaving it alone",
                         cur);
                return false;
            }
            if (!melange::mem::Expect(kCmp, {0x10, 0x73}) || !melange::mem::Expect(kMov, {0x10, 0x00, 0x00, 0x00})) {
                LOG_WARN("SmoothSixty: limiter site does not match the expected bytes; not patching");
                return false;
            }
            saved_ = cur;
            melange::mem::Put<uint8_t>(kCmp, static_cast<uint8_t>(1));
            melange::mem::Put<uint8_t>(kMov, static_cast<uint8_t>(1));
            patched_ = true;
            return true;
        }
        if (patched_) {
            melange::mem::Put<uint8_t>(kCmp, saved_);
            melange::mem::Put<uint8_t>(kMov, saved_);
            patched_ = false;
        }
        return true;
    }

    void RestoreVsync() {
        if (!vsyncSaved_ || !wglGetCurrentContext()) return;
        using SetFn = BOOL(WINAPI*)(int);
        auto set = reinterpret_cast<SetFn>(wglGetProcAddress("wglSwapIntervalEXT"));
        if (set) set(savedInterval_);
        vsyncSaved_ = false;
    }

    void EnsureVsync() {
        if (!on_ || !wglGetCurrentContext()) return;
        using GetFn = int(WINAPI*)();
        using SetFn = BOOL(WINAPI*)(int);
        static auto get = reinterpret_cast<GetFn>(wglGetProcAddress("wglGetSwapIntervalEXT"));
        static auto set = reinterpret_cast<SetFn>(wglGetProcAddress("wglSwapIntervalEXT"));
        if (!get || !set) return;
        if (!vsyncSaved_) {
            savedInterval_ = get();
            vsyncSaved_ = true;
        }
        if (get() != 1) set(1);
    }
};
}  // namespace

MELANGE_MODULE(WindowTag);
MELANGE_MODULE(FrameInterval);
MELANGE_MODULE(SmoothSixty);
