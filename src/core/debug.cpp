#include "core/debug.h"

#include <dbghelp.h>

#include <cstdio>
#include <cstring>

#include "core/dump_paths.h"
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

namespace {
std::wstring g_documentsDir;

struct HookSlot {
    CrashHook fn;
    const char* name;
};
HookSlot g_hooks[8];
LONG g_hookCount = 0;

bool CallHookGuarded(CrashHook fn, DWORD crashingThread) {
    __try {
        fn(crashingThread);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The C++ type thrown by an MSVC throw (0xE06D7363), from its ThrowInfo: the first catchable type is the thrown class.
// *isStdException: std::exception is among its catchable types, so what() is the second word of the object.
std::string ThrownTypeName(const EXCEPTION_RECORD& rec, bool* isStdException) {
    *isStdException = false;
    if (rec.NumberParameters < 3) return {};
    uintptr_t cta = 0, n = 0;
    if (!mem::SafeRead(rec.ExceptionInformation[2] + 12, &cta, 4) || !cta || !mem::SafeRead(cta, &n, 4)) return {};
    std::string first;
    for (uintptr_t i = 0; i < n && i < 16; ++i) {
        uintptr_t ct = 0, td = 0;
        char raw[128];
        if (!mem::SafeRead(cta + 4 + 4 * i, &ct, 4) || !mem::SafeRead(ct + 4, &td, 4) || !mem::SafeRead(td + 8, raw, sizeof raw))
            break;
        raw[sizeof raw - 1] = 0;
        if (strcmp(raw, ".?AVexception@std@@") == 0) *isStdException = true;
        if (i) continue;
        char out[256];
        first = raw[0] == '.' && UnDecorateSymbolName(raw + 1, out, sizeof out, 0x2800) ? out : raw;
        for (const char* prefix : {"class ", "struct "})
            if (first.rfind(prefix, 0) == 0) first.erase(0, strlen(prefix));
    }
    return first;
}
}  // namespace

void SetDocumentsDir(const std::wstring& dir) { g_documentsDir = dir; }

std::string DescribeException(const EXCEPTION_RECORD& rec) {
    char buf[384];
    const std::string where = game::DescribeAddress(reinterpret_cast<uintptr_t>(rec.ExceptionAddress));
    if (rec.ExceptionCode == 0xE06D7363) {  // MSVC C++ throw
        bool isStd = false;
        const std::string type = ThrownTypeName(rec, &isStd);
        std::string what;
        uintptr_t whatPtr = 0;
        char text[128];
        if (isStd && rec.NumberParameters >= 2 && mem::SafeRead(rec.ExceptionInformation[1] + 4, &whatPtr, 4) && whatPtr &&
            mem::SafeRead(whatPtr, text, sizeof text)) {
            text[sizeof text - 1] = 0;
            what = text;
        }
        snprintf(buf, sizeof buf, "C++ exception %s%s%s%s thrown at %s", type.empty() ? "(unknown type)" : type.c_str(),
                 what.empty() ? "" : " (\"", what.c_str(), what.empty() ? "" : "\")", where.c_str());
        return buf;
    }
    int n = snprintf(buf, sizeof buf, "%08lx at %s", rec.ExceptionCode, where.c_str());
    if ((rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec.NumberParameters >= 2 && n > 0 && n < static_cast<int>(sizeof buf)) {
        const ULONG_PTR kind = rec.ExceptionInformation[0];
        snprintf(buf + n, sizeof buf - n, ", %s of address %08lx", kind == 8 ? "execute" : kind ? "write" : "read",
                 static_cast<unsigned long>(rec.ExceptionInformation[1]));
    } else if (rec.ExceptionCode == EXCEPTION_STACK_OVERFLOW && n > 0 && n < static_cast<int>(sizeof buf)) {
        snprintf(buf + n, sizeof buf - n, " (stack overflow)");
    }
    return buf;
}

void AddCrashHook(CrashHook fn, const char* name) {
    if (!fn) return;
    const LONG i = InterlockedIncrement(&g_hookCount) - 1;
    if (i >= static_cast<LONG>(sizeof g_hooks / sizeof g_hooks[0])) {
        InterlockedDecrement(&g_hookCount);
        LOG_WARN("crash hook '%s' not added: the table is full", name ? name : "?");
        return;
    }
    g_hooks[i] = HookSlot{fn, name ? name : "?"};
}

void RunCrashHooks(DWORD crashingThread) {
    const LONG n = g_hookCount;
    for (LONG i = 0; i < n && i < static_cast<LONG>(sizeof g_hooks / sizeof g_hooks[0]); ++i) {
        if (!g_hooks[i].fn) continue;
        if (!CallHookGuarded(g_hooks[i].fn, crashingThread)) LOG_ERROR("     crash hook '%s' faulted", g_hooks[i].name);
    }
}

std::wstring WriteMiniDump(const char* tag, EXCEPTION_POINTERS* ep, DWORD crashingThread, bool full, std::string* error,
                           const char* comment) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::wstring name = DumpFileName(st, tag, full);
    MINIDUMP_EXCEPTION_INFORMATION mei{crashingThread, ep, FALSE};
    auto type = static_cast<MINIDUMP_TYPE>(
        full ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
             : (MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory));
    MINIDUMP_USER_STREAM stream{};
    MINIDUMP_USER_STREAM_INFORMATION streams{};
    if (comment && *comment) {
        stream.Type = CommentStreamA;
        stream.BufferSize = static_cast<ULONG>(strlen(comment) + 1);
        stream.Buffer = const_cast<char*>(comment);
        streams.UserStreamCount = 1;
        streams.UserStreamArray = &stream;
    }
    std::string why;
    for (const std::wstring& dir : DumpDirs(game::DataDir(), g_documentsDir)) {
        // Its parent too: Documents\Melange may not exist yet. Failures show up as CreateFileW's error below.
        CreateDirectoryW(dir.substr(0, dir.find_last_of(L'\\')).c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring path = dir + L"\\" + name;
        char line[64];
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            snprintf(line, sizeof line, ": cannot create the file (error %lu); ", GetLastError());
            why += game::Narrow(path) + line;
            continue;
        }
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &mei : nullptr,
                                          streams.UserStreamCount ? &streams : nullptr, nullptr);
        const DWORD err = ok ? 0 : GetLastError();
        CloseHandle(f);
        if (ok) return path;
        DeleteFileW(path.c_str());
        snprintf(line, sizeof line, ": MiniDumpWriteDump failed (0x%08lx); ", err);
        why += game::Narrow(path) + line;
    }
    if (why.empty()) why = "no dump folder (the data folder and Documents are both unknown); ";
    if (error) *error = why.substr(0, why.size() - 2);
    return {};
}
}  // namespace melange::debug
