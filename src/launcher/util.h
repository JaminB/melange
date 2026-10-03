#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace melange::launcher {
std::string Narrow(std::wstring_view w);
std::wstring Widen(std::string_view s);
std::string Lower(std::string s);
std::wstring LowerW(std::wstring s);
bool IEquals(std::string_view a, std::string_view b);
bool IContains(std::string_view hay, std::string_view needle);

// GetFullPathNameW, backslashes, no trailing slash (except "C:\").
std::wstring FullPath(const std::wstring& p);
// FullPath, lower-case: the key two paths are compared by.
std::wstring PathKey(const std::wstring& p);
bool PathInside(const std::wstring& path, const std::wstring& dir);   // dir itself counts
std::wstring Parent(const std::wstring& p);
std::wstring FileName(const std::wstring& p);

bool FileExists(const std::wstring& p);
bool DirExists(const std::wstring& p);
bool ReadAll(const std::wstring& p, std::string* out, size_t cap = 64u << 20);
// Temp file beside `p`, flushed, then MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH). Returns 0 or a Win32 error.
unsigned long WriteAtomic(const std::wstring& p, std::string_view data);
bool MakeDirs(const std::wstring& dir);
uint64_t FileSize(const std::wstring& p);

std::wstring ExePath();
std::wstring ExeDir();
std::wstring AppDataDir();   // %LOCALAPPDATA%\Melange, created on demand

std::string Win32Message(unsigned long code);
std::string NowIsoUtc();
std::string StampLocal();    // YYYYMMDD-HHMMSS
std::string RandomHex(int bytes);
}  // namespace melange::launcher
