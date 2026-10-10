#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

// The large-address-aware (4 GB) flag of a 32-bit exe, and the hash that ignores it. No game dependencies: the
// plugin, Melange.exe, the system report and the self-tests all link this.
namespace melange::pe {
// IMAGE_FILE_HEADER.Characteristics (the WORD at e_lfanew+22), TimeDateStamp and the optional header CheckSum.
// False when the file is not a PE. Any out pointer may be null.
bool ReadPeFlags(const std::wstring& path, uint16_t* characteristics, uint32_t* timestamp = nullptr, uint32_t* checksum = nullptr);

// SHA-256 of the file with the large-address-aware bit forced clear (Characteristics &= ~0x0020, nothing else
// touched, CheckSum included). A patched and an unpatched copy of one build give the same string, so it is the
// identity of the build. A file with no readable PE header is hashed as it is. "" when it cannot be read.
std::string CanonicalSha256(HANDLE file);
std::string CanonicalSha256(const std::wstring& path);

bool IsLaaFile(const std::wstring& path);
// Sets or clears the bit in place and flushes. Returns 0 or a Win32 error code. Unchanged when already as wanted.
unsigned long SetLaaInFile(const std::wstring& path, bool on);
}  // namespace melange::pe
