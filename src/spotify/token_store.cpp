// The DPAPI-encrypted Spotify sign-in (token_store.h).
#include "spotify/token_store.h"

#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <string>
#include <vector>

#include "core/log.h"

namespace melange::spotify::tokens {
namespace {
constexpr char kEntropy[] = "melange.spotify.v1";
constexpr DWORD kMaxFile = 64 * 1024;

void Wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

std::wstring FilePath() {
    const std::wstring d = Folder();
    return d.empty() ? std::wstring() : d + L"\\token.bin";
}
DATA_BLOB EntropyBlob() {
    DATA_BLOB e{};
    e.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    e.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
    return e;
}
}  // namespace

std::wstring Folder() {
    PWSTR p = nullptr;
    std::wstring root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) root = p;
    if (p) CoTaskMemFree(p);
    if (root.empty()) return {};
    return root + L"\\Melange\\spotify";
}

namespace {
bool MakeDirs(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2 && !MakeDirs(dir.substr(0, slash))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
}  // namespace

bool MakeFolder() { return MakeDirs(Folder()); }

bool Load(Stored* out) {
    const std::wstring path = FilePath();
    if (path.empty()) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::vector<char> enc;
    bool ok = GetFileSizeEx(f, &size) != 0 && size.QuadPart > 0 && size.QuadPart <= kMaxFile;
    if (ok) {
        enc.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, enc.data(), static_cast<DWORD>(enc.size()), &got, nullptr) && got == enc.size();
    }
    CloseHandle(f);
    if (!ok) {
        LOG_WARN("[spotify] the stored sign-in could not be read");
        return false;
    }
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(enc.data());
    in.cbData = static_cast<DWORD>(enc.size());
    DATA_BLOB entropy = EntropyBlob();
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        LOG_WARN("[spotify] the stored sign-in cannot be decrypted (another Windows user or machine?); sign in again");
        return false;
    }
    std::string text(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    Stored s;
    ok = logic::DecodeStored(text, &s);
    Wipe(text);
    if (!ok) {
        LOG_WARN("[spotify] the stored sign-in is malformed; sign in again");
        return false;
    }
    *out = std::move(s);
    return true;
}

bool Save(const Stored& s) {
    const std::wstring dir = Folder();
    if (dir.empty() || !MakeFolder()) {
        LOG_WARN("[spotify] cannot create %%LOCALAPPDATA%%\\Melange\\spotify; the sign-in is not kept");
        return false;
    }
    const std::wstring path = dir + L"\\token.bin", tmp = path + L".tmp";
    std::string text = logic::EncodeStored(s);
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(text.data());
    in.cbData = static_cast<DWORD>(text.size());
    DATA_BLOB entropy = EntropyBlob();
    DATA_BLOB enc{};
    const BOOL protectedOk = CryptProtectData(&in, L"Melange Spotify", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &enc);
    Wipe(text);
    if (!protectedOk) {
        LOG_WARN("[spotify] DPAPI refused to encrypt the sign-in (error %lu); it is not kept", GetLastError());
        return false;
    }
    bool ok = false;
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        ok = WriteFile(f, enc.pbData, enc.cbData, &w, nullptr) && w == enc.cbData && FlushFileBuffers(f);
        CloseHandle(f);
    }
    LocalFree(enc.pbData);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        LOG_WARN("[spotify] the sign-in could not be written");
        return false;
    }
    LOG_INFO("[spotify] token saved");
    return true;
}

void Delete() {
    const std::wstring path = FilePath();
    if (path.empty()) return;
    const bool had = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".tmp").c_str());
    if (had) LOG_INFO("[spotify] token removed");
}
}  // namespace melange::spotify::tokens
