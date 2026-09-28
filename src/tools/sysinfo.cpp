#include "tools/sysinfo.h"

#include <windows.h>

#include <intrin.h>
#include <winternl.h>

#pragma comment(lib, "version.lib")

#include <cctype>
#include <cstdio>

#include "core/game.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "melange/overlay.h"

#include "version.h"

namespace melange::sysinfo {
namespace {

std::string NarrowLocal(const std::wstring& w) { return melange::game::Narrow(w); }

// RtlGetVersion bypasses the GetVersionEx() app-compat shims that would otherwise report Windows 8.
std::string OsBuildString() {
    using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn && fn(&vi) == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
            return buf;
        }
    }
    return "unknown";
}

// Wine/Proton export a version string from ntdll that a native Windows ntdll never has.
std::string WineVersion() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return {};
    auto fn = reinterpret_cast<const char* (*)()>(GetProcAddress(ntdll, "wine_get_version"));
    return fn ? std::string(fn()) : std::string();
}

std::string CpuBrand() {
    int regs[4][4] = {};
    __cpuid(regs[0], 0x80000002);
    __cpuid(regs[1], 0x80000003);
    __cpuid(regs[2], 0x80000004);
    char brand[65] = {};
    memcpy(brand, regs, sizeof(regs));
    std::string s(brand);
    size_t start = s.find_first_not_of(' ');
    return start == std::string::npos ? "unknown" : s.substr(start);
}

std::string Locale() {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) > 0) return NarrowLocal(buf);
    return "unknown";
}

std::string FileVersionOf(const std::wstring& path) {
    DWORD handle = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!sz) return {};
    std::vector<char> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), handle, sz, buf.data())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &ffiLen) || !ffi) return {};
    char out[64];
    snprintf(out, sizeof(out), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
             HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return out;
}

uint32_t FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) ? fa.nFileSizeLow : 0;
}

}  // namespace

std::vector<PluginFile> DetectPlugins() {
    std::vector<PluginFile> out;
    const std::wstring& root = melange::game::GameDir();
    if (root.empty()) return out;
    // Ultimate ASI Loader also loads from plugins\ and scripts\ (WUMPatch installs WUM.Patch.asi in plugins\).
    for (const wchar_t* sub : {L"", L"plugins\\", L"scripts\\"}) {
      for (const wchar_t* pattern : {L"*.asi", L"dinput8.dll"}) {
        if (*sub && pattern[0] == L'd') continue;  // the loader itself only counts next to the exe
        std::wstring dir = root + L"\\" + sub;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring full = dir + fd.cFileName;
            PluginFile p;
            p.name = NarrowLocal(std::wstring(sub) + fd.cFileName);
            p.size = FileSizeOf(full);
            p.version = FileVersionOf(full);
            p.sha256 = hashutil::Sha256HexFile(full);
            out.push_back(std::move(p));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
      }
    }
    return out;
}

std::string PluginsJson() {
    jsonmini::Arr arr;
    for (const auto& p : DetectPlugins()) {
        jsonmini::Obj o;
        o.Str("name", p.name).UInt("size", p.size).Str("version", p.version).Str("sha256", p.sha256);
        arr.Raw(o.End());
    }
    return arr.End();
}

std::string CollectJson() {
    jsonmini::Obj os;
    os.Str("build", OsBuildString());
    std::string wine = WineVersion();
    os.Bool("wineOrProton", !wine.empty());
    if (!wine.empty()) os.Str("wineVersion", wine);

    jsonmini::Obj cpu;
    cpu.Str("brand", CpuBrand());
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    cpu.UInt("logicalCores", si.dwNumberOfProcessors);

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    jsonmini::Obj ram;
    if (GlobalMemoryStatusEx(&ms)) {
        ram.UInt("totalMB", static_cast<unsigned long long>(ms.ullTotalPhys / (1024 * 1024)));
        ram.UInt("availMB", static_cast<unsigned long long>(ms.ullAvailPhys / (1024 * 1024)));
    }

    jsonmini::Obj display;
    display.Int("width", GetSystemMetrics(SM_CXSCREEN)).Int("height", GetSystemMetrics(SM_CYSCREEN));
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) display.UInt("hz", dm.dmDisplayFrequency);

    // gl.valid stays false until the overlay has seen a GL context.
    melange::overlay::GlInfo gl = melange::overlay::Gl();
    jsonmini::Obj glj;
    glj.Bool("valid", gl.valid)
        .Str("vendor", gl.vendor)
        .Str("renderer", gl.renderer)
        .Str("version", gl.version)
        .Str("glsl", gl.glsl)
        .Int("viewportW", gl.viewportW)
        .Int("viewportH", gl.viewportH);

    const melange::game::ExeInfo& exe = melange::game::Exe();
    std::wstring exePath = melange::game::GameDir().empty() ? std::wstring() : melange::game::GameDir() + L"\\WormsMayhem.exe";
    jsonmini::Obj exej;
    exej.Str("path", NarrowLocal(exePath))
        .UInt("size", exe.fileSize)
        .UInt("timestampUtc", exe.timestamp)
        .Str("sha256", exe.sha256)
        .Str("build", exe.build)
        .Bool("known", exe.known);

    std::wstring dinput = melange::game::GameDir().empty() ? std::wstring() : melange::game::GameDir() + L"\\dinput8.dll";
    jsonmini::Obj ual;
    ual.Str("dinput8Version", FileVersionOf(dinput));

    jsonmini::Obj mods;
    mods.Raw("plugins", PluginsJson());
    bool wumpatch = false, renewation = false;
    for (const auto& p : DetectPlugins()) {
        std::string n = p.name;
        for (auto& c : n) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (n.find("wum.patch") != std::string::npos || n.find("wumpatch") != std::string::npos) wumpatch = true;
        if (n.find("renewation") != std::string::npos) renewation = true;
    }
    jsonmini::Obj detected;
    detected.Bool("wumpatch", wumpatch).Bool("renewationHD", renewation);
    mods.Raw("detected", detected.End());

    jsonmini::Obj root;
    root.Str("melangeVersion", MELANGE_VERSION)
        .Raw("os", os.End())
        .Raw("cpu", cpu.End())
        .Raw("ram", ram.End())
        .Raw("display", display.End())
        .Raw("gl", glj.End())
        .Raw("exe", exej.End())
        .Raw("ual", ual.End())
        .Str("locale", Locale())
        .Raw("mods", mods.End());
    return root.End();
}
}  // namespace melange::sysinfo
