#include "core/game.h"

#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>

#include <cstdio>
#include <vector>

#include "core/exe_profiles.h"

namespace melange::game {
namespace {
ExeInfo g_exe;
std::wstring g_gameDir, g_pluginDir, g_dataDir;

std::wstring DirOf(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

std::string Sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE h{};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD rd = 0;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &rd, nullptr) && rd) BCryptHashData(h, buf.data(), rd, 0);
        unsigned char dig[32];
        if (BCryptFinishHash(h, dig, 32, 0) == 0) {
            char s[65];
            for (int i = 0; i < 32; ++i) snprintf(s + i * 2, 3, "%02x", dig[i]);
            hex = s;
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}
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
    g_exe.sha256 = Sha256File(exePath);
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
