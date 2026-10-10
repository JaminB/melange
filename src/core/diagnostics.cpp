// Diagnostics: crash handler, main-thread hang watchdog and manual snapshot hotkey.
#include <windows.h>

#include <shlobj.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/debug.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/jlog.h"
#include "version.h"

namespace {
using SetUEF_t = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);

// The last few error exceptions each thread raised, as the vectored handler first saw them. The crash filter reads
// its own thread's copy: when an exception handler is what crashed, the fault it was handling is among them.
struct FirstChance {
    DWORD code, flags, tick, params;
    ULONG_PTR address, info[3];
    DWORD eax, ebx, ecx, edx, esi, edi, eip, esp, ebp, efl;
};
struct FirstChanceRing {
    FirstChance e[4];
    uint32_t n;
};
// Plain data, zero-initialised: no TLS constructor, nothing for the handler to wait on.
thread_local FirstChanceRing t_firstChance;

// Errors only (severity bits 11: access violations, stack overflows, C++ throws ...), not the informational
// OutputDebugString, thread-naming or breakpoint exceptions. It copies a few words and always lets the search go on,
// so the game's own __try/__except and catch blocks see every exception exactly as before.
LONG CALLBACK OnFirstChance(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xC0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    FirstChanceRing& ring = t_firstChance;
    FirstChance& f = ring.e[ring.n++ % 4];
    f.code = r->ExceptionCode;
    f.flags = r->ExceptionFlags;
    f.tick = GetTickCount();
    f.params = r->NumberParameters;
    f.address = reinterpret_cast<ULONG_PTR>(r->ExceptionAddress);
    for (DWORD i = 0; i < 3; ++i) f.info[i] = i < r->NumberParameters ? r->ExceptionInformation[i] : 0;
    const CONTEXT& c = *ep->ContextRecord;
    f.eax = c.Eax;
    f.ebx = c.Ebx;
    f.ecx = c.Ecx;
    f.edx = c.Edx;
    f.esi = c.Esi;
    f.edi = c.Edi;
    f.eip = c.Eip;
    f.esp = c.Esp;
    f.ebp = c.Ebp;
    f.efl = c.EFlags;
    return EXCEPTION_CONTINUE_SEARCH;
}

EXCEPTION_RECORD ToRecord(const FirstChance& f) {
    EXCEPTION_RECORD r{};
    r.ExceptionCode = f.code;
    r.ExceptionFlags = f.flags;
    r.ExceptionAddress = reinterpret_cast<void*>(f.address);
    r.NumberParameters = f.params < 3 ? f.params : 3;
    for (int i = 0; i < 3; ++i) r.ExceptionInformation[i] = f.info[i];
    return r;
}

CONTEXT ToContext(const FirstChance& f) {
    CONTEXT c{};
    c.Eax = f.eax;
    c.Ebx = f.ebx;
    c.Ecx = f.ecx;
    c.Edx = f.edx;
    c.Esi = f.esi;
    c.Edi = f.edi;
    c.Eip = f.eip;
    c.Esp = f.esp;
    c.Ebp = f.ebp;
    c.EFlags = f.efl;
    return c;
}

// An exception still being dispatched further up the faulting thread's stack.
struct OuterFault {
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
};

class Diagnostics final : public melange::Module {
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
        // 0: a plain access violation; 1: one inside a handler; 2: abort(); 3: an exception escaping a detached
        // thread (std::terminate); 4: a pure virtual call; 5: a std::bad_alloc out of a frame subscriber (reported,
        // the game goes on)
        selfTestCrashKind_ = Int("SelfTestCrashKind", 0);
        selfTestHangFrame_ = Int("SelfTestHangAtFrame", 0);

        // Resolved now, not on the crash path: the shell call allocates and may load modules.
        PWSTR docs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs)
            melange::debug::SetDocumentsDir(docs);
        if (docs) CoTaskMemFree(docs);
        s_kiUserExceptionDispatcher = reinterpret_cast<uintptr_t>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "KiUserExceptionDispatcher"));

        // The crash dump is written by a thread that exists before anything goes wrong: dbghelp on the faulting
        // thread can fail when that thread is deep inside a nested exception or short of stack, and a crash under the
        // loader lock could not start a new thread.
        s_dumpGo = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s_dumpDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE dumpThread = s_dumpGo && s_dumpDone ? CreateThread(nullptr, 256 * 1024, &DumpThread, nullptr, 0, nullptr)
                                                   : nullptr;
        if (dumpThread) {
            CloseHandle(dumpThread);
        } else {
            s_dumpThread = false;
            LOG_WARN("crash dump thread unavailable: a crash dump will be written by the faulting thread");
        }
        AddVectoredExceptionHandler(0, &OnFirstChance);

        s_prevFilter = SetUnhandledExceptionFilter(&OnUnhandledException);
        // The CRT's own ways to end the process, which would otherwise leave no CRASH line and no dump. The
        // terminate handler is per thread (UCRT): this covers the installing thread, and any other thread's
        // terminate reaches abort() and so the signal handler anyway.
        signal(SIGABRT, &OnAbortSignal);
        signal(SIGABRT_COMPAT, &OnAbortSignal);
        _set_purecall_handler(&OnPureCall);
        _set_invalid_parameter_handler(&OnInvalidParameter);
        std::set_terminate(&OnTerminate);
        // The game/CRT may try to replace our filter later; keep ours in front and chain to theirs.
        melange::mem::HookIAT("KERNEL32.dll", "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&HookSetUEF),
                         reinterpret_cast<void**>(&s_origSetUEF));

        if (selfTestCrashFrame_ || selfTestHangFrame_) {
            melange::events::Subscribe(melange::events::Event::Frame, [this] {
                auto f = melange::events::FrameCount();
                if (selfTestCrashFrame_ && f == static_cast<uint64_t>(selfTestCrashFrame_)) {
                    if (selfTestCrashKind_ == 1) {
                        LOG_WARN("self-test: forcing an access violation inside an exception filter (a handler fault)");
                        FaultInsideHandler();
                    }
                    if (selfTestCrashKind_ == 2) {
                        LOG_WARN("self-test: calling abort()");
                        abort();
                    }
                    if (selfTestCrashKind_ == 3) {
                        LOG_WARN("self-test: throwing out of a detached thread without the guard");
                        std::thread([] { throw std::runtime_error("self-test: unguarded thread body"); }).detach();
                        return;
                    }
                    if (selfTestCrashKind_ == 4) {
                        LOG_WARN("self-test: making a pure virtual call");
                        PureCall();
                    }
                    if (selfTestCrashKind_ == 5) {
                        LOG_WARN("self-test: throwing std::bad_alloc from a frame subscriber");
                        throw std::bad_alloc();   // caught by events::Fire: an out-of-memory ERROR, no crash
                    }
                    LOG_WARN("self-test: forcing an access violation");
                    *reinterpret_cast<volatile int*>(0) = 1;
                }
                if (selfTestHangFrame_ && f == static_cast<uint64_t>(selfTestHangFrame_)) {
                    LOG_WARN("self-test: freezing main thread for %d s", hangSeconds_ + 5);
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
    static inline uintptr_t s_kiUserExceptionDispatcher = 0;

    // One crash dump job, handed from the filter to DumpThread.
    static inline bool s_dumpThread = true;
    static inline HANDLE s_dumpGo = nullptr, s_dumpDone = nullptr;
    static inline EXCEPTION_POINTERS* s_jobEp = nullptr;
    static inline DWORD s_jobThread = 0;
    static inline char s_jobComment[768];
    static inline std::wstring s_jobPath;
    static inline std::string s_jobError;

    bool fullDumps_ = false;
    int hangSeconds_ = 8;
    bool hotkey_ = true;
    int selfTestCrashFrame_ = 0;
    int selfTestCrashKind_ = 0;
    int selfTestHangFrame_ = 0;

    // The 2026-10-05 crash's shape: a first fault, then a write to 00000014 in the filter handling it.
    static int FaultingFilter() {
        *reinterpret_cast<volatile int*>(0x14) = 1;
        return EXCEPTION_EXECUTE_HANDLER;
    }
    static void FaultInsideHandler() {
        __try {
            *reinterpret_cast<volatile int*>(0) = 1;
        } __except (FaultingFilter()) {
        }
    }

    struct PureBase {
        virtual ~PureBase() { Call(); }   // the derived part is gone by now: this reaches the pure Run
        void Call() { Run(); }
        virtual void Run() = 0;
    };
    struct PureDerived : PureBase {
        void Run() override {}
    };
    __declspec(noinline) static void PureCall() {
        PureBase* volatile b = new PureDerived;
        delete b;
    }

    static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookSetUEF(LPTOP_LEVEL_EXCEPTION_FILTER f) {
        LPTOP_LEVEL_EXCEPTION_FILTER old = s_prevFilter;
        if (f != &OnUnhandledException) {
            LOG_INFO("game installed its own crash filter %p - chaining it behind Melange", reinterpret_cast<void*>(f));
            s_prevFilter = f;
        }
        return old;
    }

    static void DoDumpJob() {
        s_jobPath = melange::debug::WriteMiniDump("crash", s_jobEp, s_jobThread, s_self && s_self->fullDumps_,
                                                  &s_jobError, s_jobComment);
        // After the dump, which suspends every other thread while it runs (e.g. a recording's writer thread).
        melange::debug::RunCrashHooks(s_jobThread);
    }
    static bool DumpJobGuarded() {
        __try {
            DoDumpJob();
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    static DWORD WINAPI DumpThread(LPVOID) {
        for (;;) {
            if (WaitForSingleObject(s_dumpGo, INFINITE) != WAIT_OBJECT_0) return 0;
            if (!DumpJobGuarded()) s_jobError = "the dump thread faulted while writing the dump";
            SetEvent(s_dumpDone);
        }
    }

    // KiUserExceptionDispatcher calls RtlDispatchException(record, context), so while an exception is being
    // dispatched its stack holds [return into KiUserExceptionDispatcher][EXCEPTION_RECORD*][CONTEXT*]. Above the
    // crash's own stack pointer (its own dispatch is below it) such a frame is an outer exception whose handler is
    // still running: the crash is a fault inside that handler. The frame must match, by code, address and stack
    // pointer, an exception the vectored handler recorded on this thread in the last 30 s: a dispatch frame left over
    // from an exception that has finished can sit in a later function's uninitialised locals and would otherwise read
    // as a live one.
    static bool FindOuterFault(uintptr_t crashEsp, const FirstChanceRing& ring, const EXCEPTION_RECORD* crash,
                               OuterFault* out) {
        const uintptr_t ki = s_kiUserExceptionDispatcher;
        if (!ki) return false;
        for (uintptr_t a = crashEsp; a < crashEsp + 0x10000; a += 4) {
            uintptr_t w[3];
            if (!melange::mem::SafeRead(a, w, sizeof w)) return false;
            if (w[0] <= ki || w[0] >= ki + 0x40) continue;
            if (!melange::mem::SafeRead(w[1], &out->rec, sizeof out->rec) ||
                !melange::mem::SafeRead(w[2], &out->ctx, sizeof out->ctx))
                continue;
            const uintptr_t at = reinterpret_cast<uintptr_t>(out->rec.ExceptionAddress);
            if (out->ctx.Esp <= a || out->rec.NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS) continue;
            if (out->rec.ExceptionCode == crash->ExceptionCode && at == reinterpret_cast<uintptr_t>(crash->ExceptionAddress))
                continue;   // the crash's own dispatch
            const DWORD now = GetTickCount();
            for (uint32_t k = 0; k < 4 && k < ring.n; ++k) {
                const FirstChance& f = ring.e[(ring.n - 1 - k) % 4];
                if (f.code == out->rec.ExceptionCode && f.address == at && f.esp == out->ctx.Esp && now - f.tick < 30000)
                    return true;
            }
        }
        return false;
    }

    static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
        if (s_inCrash.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
        ReportFatal(ep, nullptr);
        return s_prevFilter ? s_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    // abort(), a pure virtual call or an invalid-parameter report end the process with __fastfail, which skips the
    // vectored handler and the unhandled-exception filter above. Each of those paths ends up here instead, with a
    // record made up from the calling thread's own context, so the log gets a stack and the minidump is written.
    static constexpr DWORD kStatusFatalAppExit = 0x40000015;
    __declspec(noinline) static void ReportSynthetic(const char* reason) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&ctx);
        EXCEPTION_RECORD rec{};
        rec.ExceptionCode = kStatusFatalAppExit;
        rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Eip);
        EXCEPTION_POINTERS ep{&rec, &ctx};
        ReportFatal(&ep, reason);
    }

    // The exception in flight on this thread, when there is one (terminate and abort can run while it is still the
    // current exception).
    static std::string DescribeCurrentException() {
        if (!std::current_exception()) return std::string();
        try {
            throw;
        } catch (const std::exception& e) {
            return std::string("exception in flight: ") + e.what();
        } catch (...) {
            return "exception in flight: non-std exception";
        }
    }

    static void OnAbortSignal(int) {
        if (s_inCrash.exchange(true)) return;   // already reporting (or a crash was reported): let abort() go on
        LOG_ERROR("==== CRASH: abort() called on thread %lu", GetCurrentThreadId());
        const std::string cur = DescribeCurrentException();
        if (!cur.empty()) LOG_ERROR("     %s", cur.c_str());
        ReportSynthetic("abort()");
    }
    static void OnPureCall() {
        if (!s_inCrash.exchange(true)) {
            LOG_ERROR("==== CRASH: pure virtual function call on thread %lu", GetCurrentThreadId());
            ReportSynthetic("pure virtual call");
        }
        abort();
    }
    static void OnInvalidParameter(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line, uintptr_t) {
        if (!s_inCrash.exchange(true)) {
            LOG_ERROR("==== CRASH: invalid parameter passed to a CRT function on thread %lu", GetCurrentThreadId());
            if (expr || func || file)   // a release CRT passes nothing
                LOG_ERROR("     %s in %s (%s:%u)", expr ? melange::game::Narrow(expr).c_str() : "?",
                         func ? melange::game::Narrow(func).c_str() : "?", file ? melange::game::Narrow(file).c_str() : "?",
                         line);
            ReportSynthetic("invalid parameter");
        }
        abort();
    }
    static void OnTerminate() {
        if (!s_inCrash.load()) {
            LOG_ERROR("==== std::terminate called on thread %lu", GetCurrentThreadId());
            const std::string cur = DescribeCurrentException();
            if (!cur.empty()) LOG_ERROR("     %s", cur.c_str());
        }
        abort();   // reported by OnAbortSignal
    }

    // The report for a fatal event: log lines, registers and stack scan, the minidump, crash hooks. The caller has
    // claimed s_inCrash. With a reason (a synthetic record from a path above, which logged its own headline) the
    // headline is skipped.
    static void ReportFatal(EXCEPTION_POINTERS* ep, const char* reason) {
        // First, before anything below raises (and records) exceptions of its own: SafeRead in the stack scans.
        const FirstChanceRing ring = t_firstChance;
        auto* rec = ep->ExceptionRecord;
        const CONTEXT& ctx = *ep->ContextRecord;

        // The most recent recorded exception that is not this one (the vectored handler saw this one first, too).
        const FirstChance* earlier = nullptr;
        for (uint32_t k = 0; k < 4 && k < ring.n; ++k) {
            const FirstChance& f = ring.e[(ring.n - 1 - k) % 4];
            if (f.code != rec->ExceptionCode || f.address != reinterpret_cast<ULONG_PTR>(rec->ExceptionAddress) ||
                f.esp != ctx.Esp) {
                earlier = &f;
                break;
            }
        }
        OuterFault outer{};
        const bool haveOuter = FindOuterFault(ctx.Esp, ring, rec, &outer);
        const bool nestedFlag = (rec->ExceptionFlags & EXCEPTION_NESTED_CALL) != 0;
        const bool nested = haveOuter || nestedFlag;
        const DWORD ago = earlier ? GetTickCount() - earlier->tick : 0;

        if (!reason)
            LOG_ERROR("==== CRASH: exception %08lx at %s%s", rec->ExceptionCode,
                     melange::game::DescribeAddress(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)).c_str(),
                     nested ? "  [handler fault: raised while an earlier exception was being handled]" : "  [first fault]");
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
            LOG_ERROR("     %s of address %08lx", rec->ExceptionInformation[0] ? "write" : "read",
                     static_cast<unsigned long>(rec->ExceptionInformation[1]));
        else if (rec->ExceptionCode == 0xE06D7363)
            LOG_ERROR("     %s", melange::debug::DescribeException(*rec).c_str());
        if (rec->ExceptionFlags)
            LOG_ERROR("     exception flags %08lx%s", rec->ExceptionFlags, nestedFlag ? " (nested call)" : "");
        std::string detail = melange::debug::FormatRegisters(ctx) + melange::debug::ScanStack(ctx.Eip, ctx.Esp);
        melange::log::WriteRaw(detail.c_str());

        // The first fault: from the outer dispatch frame when there is one (the complete record and registers),
        // else what the vectored handler kept.
        std::string first;
        if (haveOuter) {
            first = melange::debug::DescribeException(outer.rec);
            const bool seen = earlier && earlier->code == outer.rec.ExceptionCode &&
                              earlier->address == reinterpret_cast<ULONG_PTR>(outer.rec.ExceptionAddress);
            char when[48] = "";
            if (seen) snprintf(when, sizeof when, ", %lu ms before", ago);
            LOG_ERROR("     FIRST FAULT, the exception that handler was handling (this thread%s): %s", when, first.c_str());
            std::string firstDetail =
                melange::debug::FormatRegisters(outer.ctx) + melange::debug::ScanStack(outer.ctx.Eip, outer.ctx.Esp);
            melange::log::WriteRaw(firstDetail.c_str());
        } else if (earlier) {
            const EXCEPTION_RECORD er = ToRecord(*earlier);
            first = melange::debug::DescribeException(er);
            if (nested) {
                LOG_ERROR("     FIRST FAULT, the last exception before this one (this thread, %lu ms before): %s", ago,
                         first.c_str());
                const CONTEXT ec = ToContext(*earlier);
                std::string firstDetail = melange::debug::FormatRegisters(ec) + melange::debug::ScanStack(ec.Eip, ec.Esp);
                melange::log::WriteRaw(firstDetail.c_str());
            } else {
                LOG_ERROR("     an earlier exception on this thread, %lu ms before (it was handled; may be unrelated): %s",
                         ago, first.c_str());
            }
        }
        {
            char space[200];   // fixed buffers: this path must not depend on a heap that may be what ran out
            melange::mem::FormatAddressSpace(melange::mem::QueryAddressSpace(), space, sizeof space);
            LOG_ERROR("     %s", space);
        }
        melange::jlog::FlushFromCrash();

        const std::string crashText = melange::debug::DescribeException(*rec);
        const std::string agoText = earlier ? " (" + std::to_string(ago) + " ms earlier)" : std::string();
        snprintf(s_jobComment, sizeof s_jobComment, "Melange " MELANGE_VERSION ": %s %s; %s%s%s",
                 reason ? reason : nested ? "handler fault" : "crash (first fault)", crashText.c_str(),
                 first.empty() ? "no earlier exception recorded" : nested ? "first fault " : "earlier handled exception ",
                 first.c_str(), first.empty() ? "" : agoText.c_str());

        std::wstring path;
        std::string error;
        const DWORD waitMs = s_self && s_self->fullDumps_ ? 180000 : 45000;
        if (s_dumpThread) {
            s_jobEp = ep;
            s_jobThread = GetCurrentThreadId();
            SetEvent(s_dumpGo);
            if (WaitForSingleObject(s_dumpDone, waitMs) == WAIT_OBJECT_0) {
                path = s_jobPath;
                error = s_jobError;
            } else {
                // The dump thread may be stuck on a lock this thread holds (a crash inside the heap or the loader:
                // this thread can re-enter those, another cannot), so write the dump here instead. Hooks stay with
                // the dump thread: they must not run twice if it does finish.
                error = "the dump thread did not finish within " + std::to_string(waitMs / 1000) + " s";
                std::string retryError;
                path = melange::debug::WriteMiniDump("crash-retry", ep, GetCurrentThreadId(),
                                                     s_self && s_self->fullDumps_, &retryError, s_jobComment);
                if (path.empty()) error += "; on the faulting thread: " + retryError;
            }
        } else {
            path = melange::debug::WriteMiniDump("crash", ep, GetCurrentThreadId(), s_self && s_self->fullDumps_, &error,
                                                 s_jobComment);
            melange::debug::RunCrashHooks(GetCurrentThreadId());
        }
        if (path.empty())
            LOG_ERROR("     minidump: (failed) %s", error.c_str());
        else
            LOG_ERROR("     minidump: %s", melange::game::Narrow(path).c_str());
        melange::jlog::FlushFromCrash();
    }

    // Samples the main thread's EIP several times: a spinning wait shows a small set of repeating EIPs,
    // a blocking wait shows a single EIP inside ntdll/kernel32.
    void SampleMainThread(DWORD tid) {
        std::string eips;
        for (int i = 0; i < 8; ++i) {
            CONTEXT c{};
            melange::debug::DescribeThread(tid, &c);
            eips += "    sample " + std::to_string(i) + ": " + melange::game::DescribeAddress(c.Eip) + "\r\n";
            Sleep(60);
        }
        melange::log::WriteRaw(eips.c_str());
    }

    void Snapshot(const char* tag, DWORD tid) {
        LOG_WARN("==== %s snapshot of main thread %lu (frame %llu)", tag, tid,
                static_cast<unsigned long long>(melange::events::FrameCount()));
        melange::log::WriteRaw(melange::debug::DescribeThread(tid).c_str());
        SampleMainThread(tid);
        std::string error;
        auto path = melange::debug::WriteMiniDump(tag, nullptr, 0, fullDumps_, &error);
        if (path.empty())
            LOG_WARN("     minidump: (failed) %s", error.c_str());
        else
            LOG_WARN("     minidump: %s", melange::game::Narrow(path).c_str());
    }

    // A raw thread: an exception out of the loop (its log lines allocate, and it runs when memory is low) would end
    // the game through terminate, so the loop is restarted instead.
    static DWORD WINAPI WatchdogThread(LPVOID param) {
        for (;;) {
            try {
                return WatchdogLoop(param);
            } catch (...) {
                Sleep(1000);
            }
        }
    }
    static DWORD WatchdogLoop(LPVOID param) {
        auto* self = static_cast<Diagnostics*>(param);
        bool hung = false;
        ULONGLONG hangStart = 0, lastReport = 0, nextSpaceCheck = 0;
        bool spaceLow = false, spaceLogged = false;
        for (;;) {
            Sleep(250);
            DWORD tid = melange::events::MainThreadId();
            if (!tid) continue;

            // Every ~5 s: the game is a 2 GB process, and when the largest free block gets small, allocations fail.
            if (GetTickCount64() >= nextSpaceCheck) {
                nextSpaceCheck = GetTickCount64() + 5000;
                const melange::mem::AddressSpace sp = melange::mem::QueryAddressSpace();
                char line[200];
                melange::mem::FormatAddressSpace(sp, line, sizeof line);
                if (!spaceLogged) {
                    spaceLogged = true;
                    LOG_INFO("%s", line);
                }
                if (!spaceLow && sp.largestFreeMB < 128) {
                    spaceLow = true;
                    LOG_WARN("address space low: %u MB free, largest block %u MB (WormsMayhem.exe is a 2 GB process%s); "
                             "allocations may fail and end the game",
                             sp.freeMB, sp.largestFreeMB, sp.largeAddressAware ? "" : ", not large-address-aware");
                } else if (spaceLow && sp.largestFreeMB > 192) {
                    spaceLow = false;
                }
            }

            if (self->hotkey_ && (GetAsyncKeyState(VK_F12) & 1) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                self->Snapshot("manual", tid);
            }

            ULONGLONG now = GetTickCount64();
            ULONGLONG since = now - melange::events::LastFrameTick();
            // A minimised game may legitimately stop presenting frames.
            if (!hung && IsIconic(static_cast<HWND>(melange::events::GameWindow()))) continue;
            if (!hung && since > static_cast<ULONGLONG>(self->hangSeconds_) * 1000) {
                hung = true;
                hangStart = lastReport = now;
                LOG_ERROR("==== HANG: no frame presented for %llu ms", since);
                self->Snapshot("hang", tid);
            } else if (hung && since < 1000) {
                LOG_WARN("==== HANG recovered after %llu ms", now - hangStart);
                hung = false;
            } else if (hung && now - lastReport > 30000) {
                lastReport = now;
                LOG_ERROR("==== still hung (%llu s)", (now - hangStart) / 1000);
                melange::log::WriteRaw(melange::debug::DescribeThread(tid).c_str());
            }
        }
    }
};
}  // namespace

MELANGE_MODULE(Diagnostics);
