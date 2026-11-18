#pragma once
// The game's data store, as GetData 0x696b44 / the Rm.cpp getters reach it (build #1077): root service -> data
// resource manager (GUID 0x888288) -> descriptor by name. The descriptor's value lives at *(desc+4)+0x1c; the
// descriptor is released after use. Header-only so the gameplay modules share one copy of the walk.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game/state/gamestate_internal.h"

namespace melange::gameplay::data {
constexpr uintptr_t kXomRoot = 0x966d98, kDataGuid = 0x888288;
// Value types (descriptor vtable slot 4): 0 bool, 1 int, 2 float, 3 vector, 4 string.
enum Type : int { kBool = 0, kInt = 1, kFloat = 2, kVector = 3, kString = 4 };

template <class F>
inline F Method(uintptr_t obj, size_t off) {
    using melange::gamestate::detail::Rd;
    return reinterpret_cast<F>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off));
}

// The address of the named value and its type; 0 when the store or the name is unavailable. The address stays valid
// for the match (the descriptor's value block is owned by the store).
inline uintptr_t Locate(const char* name, int* type) {
    using melange::gamestate::detail::Rd;
    const uintptr_t root = Rd<uintptr_t>(kXomRoot);
    if (!root) return 0;
    uintptr_t desc = 0, at = 0;
    __try {
        const uintptr_t svc = Method<uintptr_t(__stdcall*)(uintptr_t, uintptr_t)>(root, 0x54)(root, kDataGuid);
        if (!svc) return 0;
        const char* nm = name;
        const int rc = Method<int(__stdcall*)(uintptr_t, const char**, uintptr_t*)>(svc, 0x74)(svc, &nm, &desc);
        if (rc >= 0 && desc) {
            if (type) *type = Method<int(__stdcall*)(uintptr_t)>(desc, 0x10)(desc);
            at = Rd<uintptr_t>(desc + 4) + 0x1c;
        }
        if (desc) Method<uintptr_t(__stdcall*)(uintptr_t)>(desc, 0x8)(desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return at;
}

inline bool Read(const char* name, int wantType, void* out, size_t n) {
    int type = -1;
    const uintptr_t at = Locate(name, &type);
    return at && type == wantType && melange::gamestate::detail::Copy(at, out, n);
}

// A bool or int value as true/false; false when absent.
inline bool Flag(const char* name) {
    int type = -1;
    const uintptr_t at = Locate(name, &type);
    if (!at) return false;
    uint32_t v = 0;
    if (type == kBool) {
        uint8_t b = 0;
        melange::gamestate::detail::Copy(at, &b, 1);
        return b != 0;
    }
    if (type == kInt && melange::gamestate::detail::Copy(at, &v, 4)) return v != 0;
    return false;
}

// Raw value bytes for a save/restore around an engine call (no listeners are told; use only to put back a value the
// same call changed).
inline bool Snapshot(const char* name, uintptr_t* at, uint8_t buf[4]) {
    int type = -1;
    *at = Locate(name, &type);
    return *at && (type == kBool || type == kInt || type == kFloat) && melange::gamestate::detail::Copy(*at, buf, 4);
}
inline void Restore(uintptr_t at, const uint8_t buf[4]) {
    if (!at) return;
    __try {
        memcpy(reinterpret_cast<void*>(at), buf, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
}  // namespace melange::gameplay::data
