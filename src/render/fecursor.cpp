// The frontend cursor (menus, lobby, pause popups): the first MouseEntity's position, mapped to window pixels with the
// game's own aspect factors (FeCursorToWindow, input_logic.h). Read-only and Expect-guarded; build #1077 only.
// Test verb input.fecursor logs the raw and mapped values (the B1 corner sweep).
#include <windows.h>

#include <cstdint>
#include <string_view>

#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "render/input_logic.h"
#include "render/input_regions.h"
#include "melange/testcmd.h"

namespace melange::render {
namespace {
constexpr uintptr_t kMouseEntityList = 0x97ac94;  // MouseEntity XomClass (0x97ac60) + 0x34: the first instance, 0 = none
constexpr uintptr_t kAspectSource = 0x9795a4;     // read by 0x4d4cc0: +0x28 / +0x2c = the render width / height
constexpr uintptr_t kEntityX = 0x28, kEntityY = 0x2c;

int g_state = 0;  // 0 unchecked, 1 the code matches, -1 it does not (main thread)

bool Checked() {
    if (g_state == 0) {
        // 0x4d4cc0: sub esp,8; mov eax,[0x9795a4] (the aspect factors); 0x744a67: the clamp of the entity's position,
        // lea edi,[esi+0x28]; 0x744be4 / 0x638985: the factory links new MouseEntities into the class list (+0x34).
        const bool ok =
            mem::Expect(0x4d4cc0, {0x83, 0xec, 0x08, 0xa1, 0xa4, 0x95, 0x97, 0x00}) &&
            mem::Expect(0x744a67, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c, 0x53, 0x56, 0x8b, 0xf1, 0x57, 0x8d, 0x7e, 0x28}) &&
            mem::Expect(0x744be4, {0xb9, 0x60, 0xac, 0x97, 0x00, 0xe8, 0x97, 0x3d, 0xef, 0xff, 0xa1, 0x88, 0xac, 0x97, 0x00}) &&
            mem::Expect(0x638985, {0x8b, 0x51, 0x34, 0x8b, 0x44, 0x24, 0x04, 0x83, 0x60, 0x0c, 0x00, 0x89, 0x50, 0x08});
        g_state = ok ? 1 : -1;
        if (!ok) LOG_WARN("[input] unexpected code around the MouseEntity: wum.input.cursor() has no frontend cursor");
    }
    return g_state > 0;
}

// No C++ objects here: __try needs a frame without unwinding.
bool ReadRaw(float* fx, float* fy, int* aw, int* ah) {
    __try {
        const uint32_t e = *reinterpret_cast<volatile uint32_t*>(kMouseEntityList);
        if (!e) return false;
        *fx = *reinterpret_cast<volatile float*>(e + kEntityX);
        *fy = *reinterpret_cast<volatile float*>(e + kEntityY);
        *aw = *ah = 0;
        if (const uint32_t a = *reinterpret_cast<volatile uint32_t*>(kAspectSource)) {
            *aw = *reinterpret_cast<volatile int*>(a + 0x28);
            *ah = *reinterpret_cast<volatile int*>(a + 0x2c);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool VerbFeCursor(std::string_view, void*) {
    RECT r{};
    if (HWND hwnd = static_cast<HWND>(events::GameWindow())) GetClientRect(hwnd, &r);
    float fx = 0, fy = 0, x = 0, y = 0;
    int aw = 0, ah = 0;
    if (!Checked() || !ReadRaw(&fx, &fy, &aw, &ah)) {
        LOG_INFO("[input] fecursor: none");
        return true;
    }
    const bool mapped = FeCursorToWindow(fx, fy, aw, ah, r.right, r.bottom, &x, &y);
    LOG_INFO("[input] fecursor: raw %.2f %.2f render %dx%d window %ldx%ld -> %s %.1f %.1f", fx, fy, aw, ah, r.right,
             r.bottom, mapped ? "px" : "unmapped", x, y);
    return true;
}
}  // namespace

bool FrontendCursor(int winW, int winH, float* x, float* y) {
    float fx = 0, fy = 0;
    int aw = 0, ah = 0;
    if (!Checked() || !ReadRaw(&fx, &fy, &aw, &ah)) return false;
    return FeCursorToWindow(fx, fy, aw, ah, winW, winH, x, y);
}

void RegisterFrontendCursorVerb() { testcmd::Register("input.fecursor", &VerbFeCursor); }
}  // namespace melange::render
