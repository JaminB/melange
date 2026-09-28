#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace melange::mirage::pe {
// Calls fn(name, slot) for every by-name import of `dll` in `mod`'s import table.
template <class F>
int ForEachImport(HMODULE mod, const char* dll, F&& fn) {
    if (!mod) return 0;
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            fn(reinterpret_cast<const char*>(ibn->Name), reinterpret_cast<void**>(&iat->u1.Function));
            ++n;
        }
    }
    return n;
}

inline bool InModule(HMODULE mod, const void* p) {
    if (!mod) return false;
    auto base = reinterpret_cast<uintptr_t>(mod);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto v = reinterpret_cast<uintptr_t>(p);
    return v >= base && v < base + nt->OptionalHeader.SizeOfImage;
}
}  // namespace melange::mirage::pe
