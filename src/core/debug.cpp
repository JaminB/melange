#include "core/debug.h"

#include <dbghelp.h>

#include <cstdio>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"

namespace melange::debug {
namespace {
bool IsExecutable(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

// Is `ret` plausibly the return address of a CALL instruction?
bool LooksLikeReturnAddress(uintptr_t ret) {
    if (ret < 0x10000 || !IsExecutable(ret - 7)) return false;
    // UTF-16 text on the stack ("of" = 0x0066006f) lands inside the exe's code range; skip it.
    if ((ret & 0xFF00FF00) == 0 && (ret & 0xFF) >= 0x20 && ((ret >> 16) & 0xFF) >= 0x20) return false;
    unsigned char b[7];
    if (!mem::SafeRead(ret - 7, b, 7)) return false;
    if (b[2] == 0xE8) return true;                               // call rel32
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10) return true;      // call [mod r/m disp32] (6 bytes)
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0xD0) return true;      // call reg
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0x10) return true;      // call [reg]
    if (b[4] == 0xFF && (b[5] & 0xF8) == 0x50) return true;      // call [reg+disp8]
    if (b[3] == 0xFF && (b[4] & 0xF8) == 0x14) return true;      // call [sib+disp8] (4 bytes)
    if (b[4] == 0xFF && b[5] == 0x14) return true;               // call [sib]
    if (b[1] == 0xFF && (b[2] & 0xF8) == 0x90) return true;      // call [reg+disp32]
    return false;
}
}  // namespace

std::string ScanStack(uintptr_t eip, uintptr_t esp, size_t maxBytes, int maxFrames) {
    std::string out = "    #0  " + game::DescribeAddress(eip) + "   <- eip\r\n";
    int frames = 1;
    for (size_t off = 0; off < maxBytes && frames < maxFrames; off += 4) {
        uintptr_t v;
        if (!mem::SafeRead(esp + off, &v, 4)) {
            char note[64];
            snprintf(note, sizeof(note), "    (stack unreadable at esp+%04zx)\r\n", off);
            out += note;
            break;
        }
        if (!LooksLikeReturnAddress(v)) continue;
        char line[64];
        snprintf(line, sizeof(line), "    #%-2d [esp+%04zx] ", frames++, off);
        out += line + game::DescribeAddress(v) + "\r\n";
    }
    return out;
}

std::string FormatRegisters(const CONTEXT& c) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "    eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx\r\n    eip=%08lx esp=%08lx ebp=%08lx efl=%08lx\r\n",
             c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Eip, c.Esp, c.Ebp, c.EFlags);
    return buf;
}

std::string DescribeThread(DWORD threadId, CONTEXT* outCtx) {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!t) return "    (cannot open thread)\r\n";
    std::string out;
    if (SuspendThread(t) != static_cast<DWORD>(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t, &c)) {
            out = FormatRegisters(c) + ScanStack(c.Eip, c.Esp);
            if (outCtx) *outCtx = c;
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    return out;
}

std::string RttiName(const void* object) {
    // x86 layout: vftable[-1] -> CompleteObjectLocator { sig, offset, cdOffset, TypeDescriptor* }
    //             TypeDescriptor { pVFTable, spare, char name[] = ".?AVClassName@@" }
    uintptr_t vt, col, td;
    if (!mem::SafeRead(reinterpret_cast<uintptr_t>(object), &vt, 4) || !mem::SafeRead(vt - 4, &col, 4) ||
        !mem::SafeRead(col + 12, &td, 4))
        return {};
    char name[256];
    if (!mem::SafeRead(td + 8, name, sizeof(name))) return {};
    name[255] = 0;
    char out[256];
    if (name[0] != '.') return {};
    // Type descriptors are undecorated with UNDNAME_TYPE_ONLY (0x2000) | UNDNAME_32_BIT_DECODE (0x800).
    if (UnDecorateSymbolName(name + 1, out, sizeof(out), 0x2800)) {
        std::string s = out;
        for (const char* prefix : {"class ", "struct "})
            if (s.rfind(prefix, 0) == 0) s.erase(0, strlen(prefix));
        return s;
    }
    return name;
}

std::wstring WriteMiniDump(const char* tag, EXCEPTION_POINTERS* ep, DWORD crashingThread, bool full) {
    std::wstring dir = game::DataDir() + L"\\dumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    // "-full" is a real, load-bearing part of the name (not decoration): WriteMiniDump() is the only place that
    // knows whether a dump used MiniDumpWithFullMemory, so the log exporter (which must only bundle full dumps
    // when the user opted in, docs/m0-design.md SS5.1 Q4) tells them apart by this suffix instead of by content.
    wchar_t name[144];
    swprintf(name, 144, L"\\%04u%02u%02u_%02u%02u%02u_%S%s.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, tag, full ? L"-full" : L"");
    std::wstring path = dir + name;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};

    MINIDUMP_EXCEPTION_INFORMATION mei{crashingThread, ep, FALSE};
    auto type = static_cast<MINIDUMP_TYPE>(
        full ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
             : (MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory));
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(path.c_str());
        return {};
    }
    return path;
}
}  // namespace melange::debug
