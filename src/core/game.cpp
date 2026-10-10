#include "core/game.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <vector>

#include "core/exe_profiles.h"
#include "core/pe_laa.h"

namespace melange::game {
namespace {
ExeInfo g_exe;
std::wstring g_gameDir, g_pluginDir, g_dataDir;

std::wstring DirOf(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }
}  // namespace

void Init(void* pluginModule) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exePath = buf;
    g_gameDir = DirOf(exePath);
    GetModuleFileNameW(static_cast<HMODULE>(pluginModule), buf, MAX_PATH);
    g_pluginDir = DirOf(buf);
    g_dataDir = g_pluginDir + L"\\Melange";
    CreateDirectoryW(g_dataDir.c_str(), nullptr);

    auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_exe.timestamp = nt->FileHeader.TimeDateStamp;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(exePath.c_str(), GetFileExInfoStandard, &fa)) g_exe.fileSize = fa.nFileSizeLow;
    // The hash ignores the large-address-aware bit, so a patched exe is still the known build.
    g_exe.sha256 = pe::CanonicalSha256(exePath);
    g_exe.laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    for (const auto& p : kProfiles) {
        if (p.size == g_exe.fileSize && p.timestamp == g_exe.timestamp && g_exe.sha256 == p.sha256) {
            g_exe.known = true;
            g_exe.build = p.name;
        }
    }
}

const ExeInfo& Exe() { return g_exe; }
uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
const std::wstring& GameDir() { return g_gameDir; }
const std::wstring& PluginDir() { return g_pluginDir; }
const std::wstring& DataDir() { return g_dataDir; }

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string DescribeAddress(uintptr_t addr) {
    HMODULE mod = nullptr;
    char out[MAX_PATH + 64];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) &&
        mod) {
        char name[MAX_PATH];
        GetModuleBaseNameA(GetCurrentProcess(), mod, name, MAX_PATH);
        snprintf(out, sizeof(out), "%08x %s+0x%x", static_cast<unsigned>(addr), name,
                 static_cast<unsigned>(addr - reinterpret_cast<uintptr_t>(mod)));
    } else {
        snprintf(out, sizeof(out), "%08x ?", static_cast<unsigned>(addr));
    }
    return out;
}
}  // namespace melange::game
