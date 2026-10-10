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

// The 2 GB address space of WormsMayhem.exe (it is not large-address-aware) and what is left of it. Allocation-free.
struct AddressSpace {
    uint32_t freeMB, largestFreeMB, usedMB;
    bool largeAddressAware;
};
AddressSpace QueryAddressSpace();
// IMAGE_FILE_LARGE_ADDRESS_AWARE in the PE header of the module loaded at `base`.
bool IsLargeAddressAware(const void* base);
// "address space: free 312 MB, largest block 186 MB, ..." into buf (always terminated).
void FormatAddressSpace(const AddressSpace& s, char* buf, size_t len);
// Rate limit of out-of-memory reports: the first 3 occurrences, then every 1000th.
bool ShouldReportOom(uint32_t count);
// Logs an allocation failure caught at `where` (an ERROR with the address-space summary), rate limited. Callable from
// any thread and from a catch block: it uses fixed buffers and swallows anything it throws.
void ReportOutOfMemory(const char* where) noexcept;
}  // namespace melange::mem
