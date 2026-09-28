#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>

// Low-level memory patching helpers. Inline/mid-function hooks use SafetyHook directly (<safetyhook.hpp>).
namespace melange::mem {
bool Write(uintptr_t addr, const void* data, size_t n);
template <class T>
bool Put(uintptr_t addr, T value) {
    return Write(addr, &value, sizeof(T));
}
bool Nop(uintptr_t addr, size_t n);
bool Jmp(uintptr_t from, const void* to);  // 5-byte E9 rel32
// Verify original bytes before patching a hard-coded address. -1 = wildcard.
bool Expect(uintptr_t addr, std::initializer_list<int> bytes);

// IDA-style pattern ("75 4D 8B ?? 24 10") scan of WormsMayhem.exe's code. Returns 0 if not found.
uintptr_t Scan(const char* pattern);

// Import Address Table hooks on WormsMayhem.exe. `func` is a name, or "#N" for an ordinal import.
bool HookIAT(const char* dll, const char* func, void* hook, void** original);
void** FindIAT(const char* dll, const char* func);

// Swap one slot of a C++ vtable. Returns false if the slot already points at `hook`.
bool HookVTable(void* object, int index, void* hook, void** original);

// SEH-guarded reads for diagnostics code poking at game memory.
bool SafeRead(uintptr_t addr, void* out, size_t n);
}  // namespace melange::mem
