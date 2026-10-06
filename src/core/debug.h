#pragma once
#include <windows.h>

#include <string>

// Crash/hang forensics helpers.
namespace melange::debug {
// Heuristic call-stack: scans the stack for values that are return addresses (preceded by a CALL).
// Works without symbols and through frame-pointer-omitted game code.
std::string ScanStack(uintptr_t eip, uintptr_t esp, size_t maxBytes = 0x2000, int maxFrames = 40);

// Suspends `threadId` (must not be the calling thread), captures its context and scans its stack.
std::string DescribeThread(DWORD threadId, CONTEXT* outCtx = nullptr);

// Writes <time>_<tag>.dmp into the first of DumpDirs() (dump_paths.h) that takes it: <DataDir>\dumps, else
// Documents\Melange\dumps. `full` includes all process memory. `ep` and `crashingThread` may belong to another
// thread of this process (the crash filter hands them to the dump thread). `comment`, if set, goes into the dump's
// comment stream. Returns the path, or "" with `*error` saying what failed in each folder (the Win32 error, or the
// HRESULT MiniDumpWriteDump left).
std::wstring WriteMiniDump(const char* tag, EXCEPTION_POINTERS* ep, DWORD crashingThread, bool full,
                           std::string* error = nullptr, const char* comment = nullptr);
// The Documents folder for that fallback. Resolved once on a normal thread (Diagnostics::Install): the shell call
// allocates and can load modules, which the crash path must not.
void SetDocumentsDir(const std::wstring& dir);

// "c0000005 at 006c38d0 WormsMayhem.exe+0x2c38d0, write of address 00000014", or for a C++ exception
// "C++ exception std::length_error (\"vector too long\") thrown at 70a01234 melange.asi+0x1234". Reads the
// exception object with SafeRead only, so it can run in an exception filter.
std::string DescribeException(const EXCEPTION_RECORD& rec);
// An __except filter that keeps the exception's record for DescribeException and handles it.
inline int CopyExceptionRecord(EXCEPTION_POINTERS* ep, EXCEPTION_RECORD* out) {
    *out = *ep->ExceptionRecord;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Work for the crash path besides the dump, e.g. pushing a recording's buffered bytes to disk. Each hook runs on the
// Diagnostics dump thread (not the faulting one) after the minidump is written, fault-guarded; it must not wait on a
// lock without a timeout. Register at Install time; at most 8.
using CrashHook = void (*)(DWORD crashingThread);
void AddCrashHook(CrashHook fn, const char* name);
void RunCrashHooks(DWORD crashingThread);

std::string FormatRegisters(const CONTEXT& c);

// MSVC RTTI class name of a polymorphic object (e.g. "XSteamConnection"), or "" if unavailable.
std::string RttiName(const void* object);
}  // namespace melange::debug
