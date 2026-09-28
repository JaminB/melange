#pragma once
#include <windows.h>

#include <string>

// Crash/hang forensics helpers shared by the diagnostics module and any module that wants a snapshot.
namespace melange::debug {
// Heuristic call-stack: scans the stack for values that are return addresses (preceded by a CALL).
// Works without symbols and through frame-pointer-omitted game code.
std::string ScanStack(uintptr_t eip, uintptr_t esp, size_t maxBytes = 0x2000, int maxFrames = 40);

// Suspends `threadId` (must not be the calling thread), captures its context and scans its stack.
std::string DescribeThread(DWORD threadId, CONTEXT* outCtx = nullptr);

// Writes <DataDir>\dumps\<time>_<tag>.dmp. `full` includes all process memory (large but best for RE).
std::wstring WriteMiniDump(const char* tag, EXCEPTION_POINTERS* ep, DWORD crashingThread, bool full);

std::string FormatRegisters(const CONTEXT& c);

// MSVC RTTI class name of a polymorphic object (e.g. "XSteamConnection"), or "" if unavailable.
std::string RttiName(const void* object);
}  // namespace melange::debug
