#include "launcher/setup/exe_check.h"

#include <windows.h>

#include <map>
#include <mutex>
#include <tuple>

#include "core/exe_profiles.h"
#include "core/pe_laa.h"
#include "launcher/util.h"
#include "tools/hash.h"
#include "tools/json_mini.h"

namespace melange::launcher::setup {
std::atomic<int> g_hashCount{0};

namespace {
using CacheKey = std::tuple<std::wstring, uint64_t, uint64_t, std::string>;

std::string ProfilesKey(const std::vector<Profile>& profiles) {
    std::string k;
    for (const auto& p : profiles) k += std::to_string(p.size) + ":" + std::to_string(p.timestamp) + ":" + p.sha256 + ";";
    return k;
}
std::mutex g_mx;
std::map<CacheKey, GameCheck> g_cache;

std::string HashHandle(HANDLE f) {
    ++g_hashCount;
    return pe::CanonicalSha256(f);   // the large-address-aware bit does not change which build this is
}

bool PeTimestamp(HANDLE f, uint32_t* ts, bool* laa) {
    IMAGE_DOS_HEADER dos{};
    DWORD rd = 0;
    if (!ReadFile(f, &dos, sizeof dos, &rd, nullptr) || rd != sizeof dos || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    LARGE_INTEGER at{};
    at.QuadPart = dos.e_lfanew;
    if (dos.e_lfanew <= 0 || !SetFilePointerEx(f, at, nullptr, FILE_BEGIN)) return false;
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    if (!ReadFile(f, &sig, sizeof sig, &rd, nullptr) || rd != sizeof sig || sig != IMAGE_NT_SIGNATURE) return false;
    if (!ReadFile(f, &fh, sizeof fh, &rd, nullptr) || rd != sizeof fh) return false;
    *ts = fh.TimeDateStamp;
    *laa = (fh.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    return true;
}
}  // namespace

const std::vector<Profile>& DefaultProfiles() {
    static const std::vector<Profile> p = [] {
        std::vector<Profile> v;
        for (const auto& k : game::kProfiles) v.push_back(Profile{k.size, k.timestamp, k.sha256, k.name});
        return v;
    }();
    return p;
}

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Ok: return "ok";
        case Verdict::WrongBuild: return "wrongBuild";
        case Verdict::NoExe: return "noExe";
        case Verdict::NotFound: return "notFound";
        case Verdict::Unreadable: return "unreadable";
    }
    return "notFound";
}

void ClearExeCache() {
    std::lock_guard lk(g_mx);
    g_cache.clear();
}

GameCheck CheckExe(const std::wstring& dirIn, const std::vector<Profile>& profiles) {
    GameCheck c;
    c.path = FullPath(dirIn);
    if (c.path.empty() || !DirExists(c.path)) {
        c.verdict = Verdict::NotFound;
        return c;
    }
    const std::wstring exe = c.path + L"\\WormsMayhem.exe";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        c.verdict = Verdict::NoExe;
        return c;
    }
    const uint64_t size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    const uint64_t mtime = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    const CacheKey key{PathKey(exe), size, mtime, ProfilesKey(profiles)};
    {
        std::lock_guard lk(g_mx);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) return it->second;
    }
    c.exe.present = true;
    c.exe.size = size;
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        c.verdict = Verdict::Unreadable;
        c.error = Win32Message(GetLastError());
        return c;
    }
    uint32_t ts = 0;
    const bool isPe = PeTimestamp(f, &ts, &c.exe.laa);
    c.exe.timestamp = ts;
    const Profile* match = nullptr;
    if (isPe)
        for (const auto& p : profiles)
            if (p.size == size && p.timestamp == ts) match = &p;
    c.verdict = Verdict::WrongBuild;
    if (match) {
        c.exe.sha256 = HashHandle(f);
        if (c.exe.sha256.empty()) {
            c.verdict = Verdict::Unreadable;
            c.error = Win32Message(GetLastError());
        } else {
            for (const auto& p : profiles)
                if (p.size == size && p.timestamp == ts && p.sha256 == c.exe.sha256) {
                    c.verdict = Verdict::Ok;
                    c.exe.build = p.name;
                }
        }
    }
    CloseHandle(f);
    if (c.verdict != Verdict::Unreadable) {
        std::lock_guard lk(g_mx);
        g_cache[key] = c;
    }
    return c;
}

bool CanWrite(const std::wstring& dir) {
    HANDLE h = CreateFileW(dir.c_str(), FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::string GameCheckJson(const GameCheck& c) {
    jsonmini::Obj o;
    o.Str("path", Narrow(c.path)).Str("verdict", VerdictName(c.verdict)).Str("store", c.store).Bool("running", c.running)
        .Bool("writable", c.writable);
    if (c.exe.present) {
        jsonmini::Obj e;
        e.UInt("size", c.exe.size).UInt("timestamp", c.exe.timestamp).Bool("laa", c.exe.laa);
        if (!c.exe.sha256.empty()) e.Str("sha256", c.exe.sha256);
        if (!c.exe.build.empty()) e.Str("build", c.exe.build);
        o.Raw("exe", e.End());
    }
    if (!c.error.empty()) o.Str("error", c.error);
    return o.End();
}
}  // namespace melange::launcher::setup
