#include "core/events.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "core/mem.h"

namespace melange::events {
namespace {
std::vector<Callback> g_subs[static_cast<int>(Event::Count)];
std::recursive_mutex g_mutex;

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_lastFrame{0};
std::atomic<DWORD> g_mainThread{0};
std::atomic<HWND> g_window{nullptr};

using SwapBuffers_t = BOOL(WINAPI*)(HDC);
SwapBuffers_t g_origSwapBuffers = nullptr;

std::atomic<PresentHook> g_presentHook{nullptr};

BOOL WINAPI HookSwapBuffers(HDC dc) {
    if (g_frames.load(std::memory_order_relaxed) == 0) {
        g_mainThread = GetCurrentThreadId();
        g_window = WindowFromDC(dc);
        LOG_INFO("first frame: main thread %lu, window %p", GetCurrentThreadId(), static_cast<void*>(g_window.load()));
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_lastFrame.store(GetTickCount64(), std::memory_order_relaxed);
    Fire(Event::Frame);
    if (PresentHook hook = g_presentHook.load(std::memory_order_acquire)) hook(dc);
    return g_origSwapBuffers(dc);
}
}  // namespace

void Subscribe(Event e, Callback cb) {
    std::lock_guard lk(g_mutex);
    g_subs[static_cast<int>(e)].push_back(std::move(cb));
}

void Fire(Event e) {
    std::lock_guard lk(g_mutex);
    if (e != Event::Frame) LOG_INFO("event %s", NameOf(e));
    for (auto& cb : g_subs[static_cast<int>(e)]) cb();
}

const char* NameOf(Event e) {
    static const char* names[] = {"Frame", "Shutdown", "MatchStart", "MatchEnd", "LobbyEnter", "LobbyLeave"};
    return names[static_cast<int>(e)];
}

void InstallCore() {
    if (mem::HookIAT("GDI32.dll", "SwapBuffers", reinterpret_cast<void*>(&HookSwapBuffers),
                     reinterpret_cast<void**>(&g_origSwapBuffers)))
        LOG_INFO("core: frame hook installed (gdi32!SwapBuffers)");
    else
        LOG_ERROR("core: frame hook FAILED - watchdog and per-frame modules will not work");
}

uint64_t FrameCount() { return g_frames.load(); }
uint64_t LastFrameTick() { return g_lastFrame.load(); }
unsigned long MainThreadId() { return g_mainThread.load(); }
void* GameWindow() { return g_window.load(); }

void SetPresentHook(PresentHook fn) { g_presentHook.store(fn, std::memory_order_release); }
}  // namespace melange::events
