// A player's frame section and the security of every helper-facing object (shm.h).
#include "media/shm.h"

#include <windows.h>
#include <sddl.h>

#include <cstdio>
#include <memory>

namespace melange::media::shmio {
namespace {
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The current user's SID as a string ("S-1-5-21-...").
bool UserSid(std::wstring* out) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::unique_ptr<uint8_t[]> buf(new uint8_t[n ? n : 1]);
    bool ok = n && GetTokenInformation(tok, TokenUser, buf.get(), n, &n);
    CloseHandle(tok);
    if (!ok) return false;
    LPWSTR s = nullptr;
    ok = ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.get())->User.Sid, &s);
    if (ok) {
        *out = s;
        LocalFree(s);
    }
    return ok;
}
}  // namespace

Security::~Security() {
    if (sd_) LocalFree(sd_);
}

bool Security::Build(bool lowLabel, std::string* err) {
    std::wstring sid;
    if (!UserSid(&sid)) {
        *err = "no user SID (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    // Protected DACL: the user and SYSTEM only. The Low label lets a Low-integrity process of the same user write;
    // without it the default (Medium, no-write-up) would lock the sandboxed helper out.
    std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;SY)";
    if (lowLabel) sddl += L"S:(ML;;NW;;;LW)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        *err = "security descriptor (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (sd_) LocalFree(sd_);
    sd_ = sd;
    static_assert(sizeof(sa_) == sizeof(SECURITY_ATTRIBUTES));
    sa_.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa_.lpSecurityDescriptor = sd_;
    sa_.bInheritHandle = FALSE;
    return true;
}

std::string SectionName(uint32_t pid, const std::string& hex, uint32_t playerId) {
    char buf[96];
    snprintf(buf, sizeof buf, "Local\\melange-holo-%u-%s-%u", pid, hex.c_str(), playerId);
    return buf;
}

Section::~Section() {
    delete reader_;
    if (base_) UnmapViewOfFile(base_);
    if (map_) CloseHandle(map_);
}

bool Section::Create(const std::string& name, bool lowLabel, std::string* err) {
    Security sec;
    if (!sec.Build(lowLabel, err)) return false;
    constexpr uint64_t bytes = shm::SectionBytes();
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, static_cast<SECURITY_ATTRIBUTES*>(sec.Attributes()), PAGE_READWRITE,
                                  static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes & 0xFFFFFFFFu), Wide(name).c_str());
    if (!h) {
        *err = "CreateFileMapping (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {  // someone else made it: never use a section we did not create
        CloseHandle(h);
        *err = "section name already exists";
        return false;
    }
    void* base = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(bytes));
    if (!base) {
        *err = "MapViewOfFile (" + std::to_string(GetLastError()) + ")";
        CloseHandle(h);
        return false;
    }
    shm::InitHeader(base);
    map_ = h;
    base_ = base;
    name_ = name;
    reader_ = new shm::Reader(base_);  // the creation limits, never the header's
    return true;
}
}  // namespace melange::media::shmio
