#include "core/mem.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/game.h"
#include "core/log.h"

namespace wf::mem {
bool Write(uintptr_t addr, const void* data, size_t n) {
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(addr), data, n);
    VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    return true;
}

bool Nop(uintptr_t addr, size_t n) {
    std::vector<unsigned char> nops(n, 0x90);
    return Write(addr, nops.data(), n);
}

bool Jmp(uintptr_t from, const void* to) {
    unsigned char b[5] = {0xE9};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (from + 5));
    memcpy(b + 1, &rel, 4);
    return Write(from, b, 5);
}

bool Expect(uintptr_t addr, std::initializer_list<int> bytes) {
    unsigned char actual[64];
    size_t n = bytes.size() < sizeof(actual) ? bytes.size() : sizeof(actual);
    if (!SafeRead(addr, actual, n)) return false;
    size_t i = 0;
    for (int b : bytes) {
        if (i >= n) break;
        if (b >= 0 && actual[i] != static_cast<unsigned char>(b)) {
            WF_WARN("byte mismatch at %08x+%zu: expected %02x got %02x", static_cast<unsigned>(addr), i, b, actual[i]);
            return false;
        }
        ++i;
    }
    return true;
}

uintptr_t Scan(const char* pattern) {
    std::vector<int> pat;
    for (const char* s = pattern; *s;) {
        if (*s == ' ') {
            ++s;
        } else if (*s == '?') {
            pat.push_back(-1);
            while (*s == '?') ++s;
        } else {
            char hex[3] = {s[0], s[1], 0};
            pat.push_back(static_cast<int>(strtoul(hex, nullptr, 16)));
            s += 2;
        }
    }
    auto base = reinterpret_cast<const unsigned char*>(game::Base());
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const unsigned char* start = base + sec->VirtualAddress;
        size_t len = sec->Misc.VirtualSize;
        for (size_t o = 0; o + pat.size() <= len; ++o) {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || start[o + k] == pat[k])) ++k;
            if (k == pat.size()) return reinterpret_cast<uintptr_t>(start + o);
        }
    }
    return 0;
}

void** FindIAT(const char* dll, const char* func) {
    auto base = reinterpret_cast<unsigned char*>(game::Base());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    bool byOrdinal = func[0] == '#';
    WORD ordinal = byOrdinal ? static_cast<WORD>(atoi(func + 1)) : 0;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                if (byOrdinal && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal) return reinterpret_cast<void**>(&iat->u1.Function);
            } else if (!byOrdinal) {
                auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<char*>(ibn->Name), func) == 0) return reinterpret_cast<void**>(&iat->u1.Function);
            }
        }
    }
    return nullptr;
}

bool HookIAT(const char* dll, const char* func, void* hook, void** original) {
    void** slot = FindIAT(dll, func);
    if (!slot) {
        WF_WARN("IAT hook: %s!%s is not imported by the game", dll, func);
        return false;
    }
    if (*slot == hook) return true;
    if (original) *original = *slot;
    return Write(reinterpret_cast<uintptr_t>(slot), &hook, sizeof(hook));
}

bool HookVTable(void* object, int index, void* hook, void** original) {
    void** vt = *static_cast<void***>(object);
    if (vt[index] == hook) return false;
    if (original) *original = vt[index];
    return Write(reinterpret_cast<uintptr_t>(&vt[index]), &hook, sizeof(hook));
}

bool SafeRead(uintptr_t addr, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace wf::mem
