// Diagnostics: crash handler, main-thread hang watchdog and manual snapshot hotkey.
// This is what turns "the game froze" into a stack trace + minidump we can analyse.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "core/debug.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "wumfix/jlog.h"  // component C's only edit to Diagnostics: flush the JSONL queue from the crash filter

namespace {
using SetUEF_t = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);

class Diagnostics final : public wf::Module {
public:
    const char* Name() const override { return "Diagnostics"; }
    const char* Description() const override { return "crash dumps, hang watchdog, Ctrl+Shift+F12 snapshot"; }
    int Order() const override { return 0; }

    bool Install() override {
        s_self = this;
        fullDumps_ = Bool("FullMemoryDumps", false);
        hangSeconds_ = Int("HangSeconds", 8);
        hotkey_ = Bool("SnapshotHotkey", true);
        selfTestCrashFrame_ = Int("SelfTestCrashAtFrame", 0);
        selfTestHangFrame_ = Int("SelfTestHangAtFrame", 0);

        s_prevFilter = SetUnhandledExceptionFilter(&OnUnhandledException);
        // The game/CRT may try to replace our filter later; keep ours in front and chain to theirs.
        wf::mem::HookIAT("KERNEL32.dll", "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&HookSetUEF),
                         reinterpret_cast<void**>(&s_origSetUEF));

        if (selfTestCrashFrame_ || selfTestHangFrame_) {
            wf::events::Subscribe(wf::events::Event::Frame, [this] {
                auto f = wf::events::FrameCount();
                if (selfTestCrashFrame_ && f == static_cast<uint64_t>(selfTestCrashFrame_)) {
                    WF_WARN("self-test: forcing an access violation");
                    *reinterpret_cast<volatile int*>(0) = 1;
                }
                if (selfTestHangFrame_ && f == static_cast<uint64_t>(selfTestHangFrame_)) {
                    WF_WARN("self-test: freezing main thread for %d s", hangSeconds_ + 5);
                    Sleep((hangSeconds_ + 5) * 1000);
                }
            });
        }

        CreateThread(nullptr, 0, &WatchdogThread, this, 0, nullptr);
        return true;
    }

private:
    static inline Diagnostics* s_self = nullptr;
    static inline LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter = nullptr;
    static inline SetUEF_t s_origSetUEF = nullptr;
    static inline std::atomic<bool> s_inCrash{false};

    bool fullDumps_ = false;
    int hangSeconds_ = 8;
    bool hotkey_ = true;
    int selfTestCrashFrame_ = 0;
    int selfTestHangFrame_ = 0;

    static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookSetUEF(LPTOP_LEVEL_EXCEPTION_FILTER f) {
        LPTOP_LEVEL_EXCEPTION_FILTER old = s_prevFilter;
        if (f != &OnUnhandledException) {
            WF_INFO("game installed its own crash filter %p - chaining it behind WUMFix", reinterpret_cast<void*>(f));
            s_prevFilter = f;
        }
        return old;
    }

    static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
        if (s_inCrash.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
        auto* rec = ep->ExceptionRecord;
        WF_ERROR("==== CRASH: exception %08lx at %s", rec->ExceptionCode,
                 wf::game::DescribeAddress(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)).c_str());
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
            WF_ERROR("     %s of address %08lx", rec->ExceptionInformation[0] ? "write" : "read",
                     static_cast<unsigned long>(rec->ExceptionInformation[1]));
        std::string detail = wf::debug::FormatRegisters(*ep->ContextRecord) +
                             wf::debug::ScanStack(ep->ContextRecord->Eip, ep->ContextRecord->Esp);
        wf::log::WriteRaw(detail.c_str());
        wf::jlog::FlushFromCrash();
        auto path = wf::debug::WriteMiniDump("crash", ep, GetCurrentThreadId(), s_self && s_self->fullDumps_);
        WF_ERROR("     minidump: %s", path.empty() ? "(failed)" : wf::game::Narrow(path).c_str());
        return s_prevFilter ? s_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    // Samples the main thread's EIP several times: a spinning wait shows a small set of repeating EIPs,
    // a blocking wait shows a single EIP inside ntdll/kernel32.
    void SampleMainThread(DWORD tid) {
        std::string eips;
        for (int i = 0; i < 8; ++i) {
            CONTEXT c{};
            wf::debug::DescribeThread(tid, &c);
            eips += "    sample " + std::to_string(i) + ": " + wf::game::DescribeAddress(c.Eip) + "\r\n";
            Sleep(60);
        }
        wf::log::WriteRaw(eips.c_str());
    }

    void Snapshot(const char* tag, DWORD tid) {
        WF_WARN("==== %s snapshot of main thread %lu (frame %llu)", tag, tid,
                static_cast<unsigned long long>(wf::events::FrameCount()));
        wf::log::WriteRaw(wf::debug::DescribeThread(tid).c_str());
        SampleMainThread(tid);
        auto path = wf::debug::WriteMiniDump(tag, nullptr, 0, fullDumps_);
        WF_WARN("     minidump: %s", path.empty() ? "(failed)" : wf::game::Narrow(path).c_str());
    }

    static DWORD WINAPI WatchdogThread(LPVOID param) {
        auto* self = static_cast<Diagnostics*>(param);
        bool hung = false;
        ULONGLONG hangStart = 0, lastReport = 0;
        for (;;) {
            Sleep(250);
            DWORD tid = wf::events::MainThreadId();
            if (!tid) continue;

            if (self->hotkey_ && (GetAsyncKeyState(VK_F12) & 1) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                self->Snapshot("manual", tid);
            }

            ULONGLONG now = GetTickCount64();
            ULONGLONG since = now - wf::events::LastFrameTick();
            // A minimised game may legitimately stop presenting frames.
            if (!hung && IsIconic(static_cast<HWND>(wf::events::GameWindow()))) continue;
            if (!hung && since > static_cast<ULONGLONG>(self->hangSeconds_) * 1000) {
                hung = true;
                hangStart = lastReport = now;
                WF_ERROR("==== HANG: no frame presented for %llu ms", since);
                self->Snapshot("hang", tid);
            } else if (hung && since < 1000) {
                WF_WARN("==== HANG recovered after %llu ms", now - hangStart);
                hung = false;
            } else if (hung && now - lastReport > 30000) {
                lastReport = now;
                WF_ERROR("==== still hung (%llu s)", (now - hangStart) / 1000);
                wf::log::WriteRaw(wf::debug::DescribeThread(tid).c_str());
            }
        }
    }
};
}  // namespace

WUMFIX_MODULE(Diagnostics);
