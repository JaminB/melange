// The in-game Spotify Client ID (client_store.h).
#include "spotify/client_store.h"

#include <windows.h>

#include <string>

#include "core/log.h"
#include "spotify/spotify_logic.h"
#include "spotify/token_store.h"

namespace melange::spotify::clientfile {
namespace {
constexpr DWORD kMaxFile = 4096;

std::wstring FilePath() {
    const std::wstring d = tokens::Folder();
    return d.empty() ? std::wstring() : d + L"\\client.json";
}
}  // namespace

bool Load(std::string* id) {
    const std::wstring path = FilePath();
    if (path.empty()) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string text;
    bool ok = GetFileSizeEx(f, &size) != 0 && size.QuadPart > 0 && size.QuadPart <= kMaxFile;
    if (ok) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) && got == text.size();
    }
    CloseHandle(f);
    std::string v;
    if (!ok || !logic::DecodeClientFile(text, &v)) {
        LOG_WARN("[spotify] %%LOCALAPPDATA%%\\Melange\\spotify\\client.json is not a valid Client ID file; it is ignored");
        return false;
    }
    *id = std::move(v);
    return true;
}

bool Save(const std::string& id) {
    if (!logic::ValidClientId(id)) return false;
    const std::wstring dir = tokens::Folder();
    if (dir.empty() || !tokens::MakeFolder()) {
        LOG_WARN("[spotify] cannot create %%LOCALAPPDATA%%\\Melange\\spotify; the Client ID is not kept");
        return false;
    }
    const std::wstring path = dir + L"\\client.json", tmp = path + L".tmp";
    const std::string text = logic::EncodeClientFile(id);
    bool ok = false;
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &w, nullptr) && w == text.size() && FlushFileBuffers(f);
        CloseHandle(f);
    }
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();  // before DeleteFileW overwrites it
        DeleteFileW(tmp.c_str());
        LOG_WARN("[spotify] the Client ID could not be written (error %lu)", err);
        return false;
    }
    return true;
}

bool Delete() {
    const std::wstring path = FilePath();
    if (path.empty()) return false;
    DeleteFileW((path + L".tmp").c_str());
    if (DeleteFileW(path.c_str())) return true;
    const DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}
}  // namespace melange::spotify::clientfile
