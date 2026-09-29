// oasis.exe: the same Oasis app and protocol, served with the game closed. Reads logs, captures and mods\ini
// straight from disk; refuses to write mods\ini while a real Melange instance holds its game-folder mutex.
#include <winsock2.h>
#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#include "version.h"

#include "melange/oasis.h"
#include "oasis/core/files.h"
#include "oasis/core/http.h"
#include "oasis/core/router.h"
#include "oasis/core/server.h"
#include "oasis/providers.h"
#include "oasis/rpc/ini_edit.h"
#include "oasis/standalone/game_lock.h"
#include "oasis/standalone/ini_edit.h"
#include "oasis/standalone/mods_provider.h"
#include "oasis/standalone/wormsign_provider.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace oa = melange::oasis;
namespace oc = melange::oasis::core;
namespace standalone = melange::oasis::standalone;
namespace jsonmini = melange::jsonmini;

namespace {
std::wstring g_gameDir;

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? s : s.substr(0, slash);
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool ReadWhole(const std::wstring& path, std::string* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize(n < 0 ? 0 : static_cast<size_t>(n));
    const bool ok = out->empty() || fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

std::wstring IniPath() { return g_gameDir + L"\\Melange.ini"; }

std::string IniGet(const char* section, const char* key, const char* def) {
    std::string text;
    if (!ReadWhole(IniPath(), &text)) return def;
    const std::string v = standalone::ini::Get(text, section, key);
    return v.empty() ? def : v;
}

bool GameRunningNow() { return standalone::GameRunning(g_gameDir); }

// ---------------------------------------------------------------- log.sessions, /logs/
bool LooksLikeSession(const std::wstring& name) {
    if (name.size() < 20) return false;
    static const int kDigitAt[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (int i : kDigitAt)
        if (!iswdigit(name[static_cast<size_t>(i)])) return false;
    return true;
}

std::wstring LogsDir() {
    const std::string dir = IniGet("Logging", "Dir", "");
    if (!dir.empty()) {
        const std::wstring w = Widen(dir);
        return w.size() > 1 && w[1] == L':' ? w : g_gameDir + L"\\" + w;
    }
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = std::wstring(docs) + L"\\Melange\\logs";
    if (docs) CoTaskMemFree(docs);
    if (out.empty() || GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES) out = g_gameDir + L"\\Melange\\logs";
    return out;
}

void LogSessions(const oa::Call&, oa::Result& r, void*) {
    const std::wstring root = LogsDir();
    jsonmini::Arr sessions;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L".." || !LooksLikeSession(name)) continue;
            jsonmini::Arr files;
            uint64_t bytes = 0;
            WIN32_FIND_DATAW ffd{};
            HANDLE fh = FindFirstFileW((root + L"\\" + name + L"\\*").c_str(), &ffd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    files.Str(Narrow(ffd.cFileName));
                    bytes += (static_cast<uint64_t>(ffd.nFileSizeHigh) << 32) | ffd.nFileSizeLow;
                } while (FindNextFileW(fh, &ffd));
                FindClose(fh);
            }
            sessions.Raw(jsonmini::Obj().Str("id", Narrow(name)).Raw("files", files.End()).UInt("bytes", bytes).End());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    r.json = sessions.End();
}

// /logs/<session>/<file>: as the in-game route, the session must be a real session folder under the
// (ini-configurable) logs directory and the file a plain name directly inside it; no further '/' or '\', no
// nested folders. A configurable Dir must not turn this into a browser for the rest of the disk.
bool RouteLogs(const oc::Request& rq, oc::Response* out, void*) {
    constexpr size_t kPrefixLen = 6;  // "/logs/"
    if (rq.path.size() <= kPrefixLen) return false;
    const std::string rel = rq.path.substr(kPrefixLen);
    const size_t slash = rel.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rel.size()) return false;
    if (rel.find('/', slash + 1) != std::string::npos) return false;
    const std::wstring session = Widen(rel.substr(0, slash)), file = Widen(rel.substr(slash + 1));
    if (!LooksLikeSession(session)) return false;
    if (file.find_first_of(L"/\\:") != std::wstring::npos || file == L"." || file == L"..") return false;
    const std::wstring root = LogsDir();
    const DWORD sessionAttr = GetFileAttributesW((root + L"\\" + session).c_str());
    if (sessionAttr == INVALID_FILE_ATTRIBUTES || !(sessionAttr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const std::wstring full = root + L"\\" + session + L"\\" + file;
    const DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    out->status = 200;
    out->contentType = oc::MimeType(rel);
    out->file = full;
    return true;
}

// ---------------------------------------------------------------- capture.list, /captures/
std::wstring CapturesDir() {
    const std::string dir = IniGet("MirageTrace", "CaptureDir", "");
    if (!dir.empty()) {
        const std::wstring w = Widen(dir);
        return w.size() > 1 && w[1] == L':' ? w : g_gameDir + L"\\" + w;
    }
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = std::wstring(docs) + L"\\Melange\\captures";
    if (docs) CoTaskMemFree(docs);
    return out.empty() ? g_gameDir + L"\\Melange\\captures" : out;
}

void CaptureList(const oa::Call&, oa::Result& r, void*) {
    const std::wstring root = CapturesDir();
    jsonmini::Arr arr;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*.mcap").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ULARGE_INTEGER t{};
            t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            const uint64_t bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            arr.Raw(jsonmini::Obj().Str("name", Narrow(fd.cFileName)).UInt("bytes", bytes).UInt("time", t.QuadPart).End());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    r.json = arr.End();
}

// /captures/<name>.mcap: flat, no subfolders, and the name must be one capture.list would report.
bool RouteCaptures(const oc::Request& rq, oc::Response* out, void*) {
    constexpr size_t kPrefixLen = 10;  // "/captures/"
    if (rq.path.size() <= kPrefixLen) return false;
    const std::string name = rq.path.substr(kPrefixLen);
    if (name.find('/') != std::string::npos || name.size() < 6 || name.substr(name.size() - 5) != ".mcap") return false;
    if (!oc::SafePath(name)) return false;
    const std::wstring full = CapturesDir() + L"\\" + Widen(name);
    const DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    out->status = 200;
    out->contentType = oc::MimeType(name);
    out->file = full;
    return true;
}

// ---------------------------------------------------------------- wormsign.library, /replays/
std::wstring ReplaysDir() {
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = std::wstring(docs) + L"\\Melange\\replays";
    if (docs) CoTaskMemFree(docs);
    return out.empty() ? g_gameDir + L"\\Melange\\replays" : out;
}

void WormsignLibrary(const oa::Call&, oa::Result& r, void*) { r.json = standalone::wormsignprov::ListJson(ReplaysDir()); }

// /replays/<name>.wsr|.zip: flat, no subfolders, as /captures/. arm/control/pin/detail are not offered here (no
// live session to arm): the library and the timeline/diff viewers work from the file alone.
bool RouteReplays(const oc::Request& rq, oc::Response* out, void*) {
    constexpr size_t kPrefixLen = 9;  // "/replays/"
    if (rq.path.size() <= kPrefixLen) return false;
    const std::string name = rq.path.substr(kPrefixLen);
    if (name.find('/') != std::string::npos || name.size() < 5) return false;
    const std::string ext = name.substr(name.size() - 4);
    if ((ext != ".wsr" && ext != ".zip") || !oc::SafePath(name)) return false;
    const std::wstring full = ReplaysDir() + L"\\" + Widen(name);
    const DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    out->status = 200;
    out->contentType = ext == ".zip" ? "application/zip" : "application/octet-stream";
    out->file = full;
    return true;
}

// ---------------------------------------------------------------- mods.list, mods.setEnabled
void ModsList(const oa::Call&, oa::Result& r, void*) { r.json = standalone::modsprov::ListJson(g_gameDir, MELANGE_VERSION); }

void ModsSetEnabled(const oa::Call& c, oa::Result& r, void*) {
    if (GameRunningNow()) {
        r.ok = false;
        r.code = -32003;
        r.message = "the game is running; mods are read-only here";
        return;
    }
    melange::json::Value p;
    melange::json::Error e;
    if (!melange::json::Parse(c.paramsJson, &p, &e) || !p.IsObject()) {
        r.ok = false;
        r.code = -32602;
        r.message = "bad params";
        return;
    }
    const melange::json::Value *id = p.Get("id"), *on = p.Get("on");
    if (!id || !id->IsString() || !on || !on->IsBool()) {
        r.ok = false;
        r.code = -32602;
        r.message = "expected {id, on}";
        return;
    }
    const int rc = standalone::modsprov::SetEnabled(g_gameDir, id->string, on->boolean);
    if (rc == 0) {
        r.ok = false;
        r.code = -32602;
        r.message = "no such mod";
        return;
    }
    if (rc < 0) {
        r.ok = false;
        r.code = -32000;
        r.message = "could not write thumper-state.json";
        return;
    }
    r.json = "true";
}

// ---------------------------------------------------------------- ini.get, ini.set
void IniGetMethod(const oa::Call&, oa::Result& r, void*) {
    std::string text;
    ReadWhole(IniPath(), &text);
    for (const auto& e : melange::oasis::ini::Parse(text))
        if (_stricmp(e.section.c_str(), "Thumper") == 0 && _stricmp(e.key.c_str(), "GrantSalt") == 0)
            text = melange::oasis::ini::Set(text, e.section, e.key, "********");
    r.json = jsonmini::Obj().Str("path", Narrow(IniPath())).Str("text", text).Raw("keys", "[]").End();
}

void IniSetMethod(const oa::Call& c, oa::Result& r, void*) {
    if (GameRunningNow()) {
        r.ok = false;
        r.code = -32003;
        r.message = "the game is running; settings are read-only here";
        return;
    }
    melange::json::Value p;
    melange::json::Error e;
    if (!melange::json::Parse(c.paramsJson, &p, &e) || !p.IsObject()) {
        r.ok = false;
        r.code = -32602;
        r.message = "bad params";
        return;
    }
    const melange::json::Value *section = p.Get("section"), *key = p.Get("key"), *value = p.Get("value");
    if (!section || !section->IsString() || !key || !key->IsString() || !value || !value->IsString()) {
        r.ok = false;
        r.code = -32602;
        r.message = "expected {section, key, value}";
        return;
    }
    if (std::string why; !melange::oasis::ini::ValidName(section->string, &why) || !melange::oasis::ini::ValidName(key->string, &why) ||
                          !melange::oasis::ini::ValidValue(value->string, &why)) {
        r.ok = false;
        r.code = -32602;
        r.message = why;
        return;
    }
    if (std::string why; melange::oasis::ini::Protected(section->string, key->string, value->string, &why)) {
        r.ok = false;
        r.code = -32000;
        r.message = why;
        return;
    }
    std::string text;
    ReadWhole(IniPath(), &text);
    std::string out;
    if (!standalone::ini::Set(text, section->string, key->string, value->string, &out)) {
        r.ok = false;
        r.code = -32602;
        r.message = "value cannot contain a line break or ';'";
        return;
    }
    FILE* f = _wfopen((IniPath() + L".tmp").c_str(), L"wb");
    bool ok = f != nullptr;
    if (f) {
        ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
    }
    if (ok) ok = MoveFileExW((IniPath() + L".tmp").c_str(), IniPath().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) {
        DeleteFileW((IniPath() + L".tmp").c_str());
        r.ok = false;
        r.code = -32000;
        r.message = "could not write Melange.ini";
        return;
    }
    r.json = jsonmini::Obj().Bool("live", false).Bool("restart", true).End();
}

std::atomic<bool> g_stop{false};
BOOL WINAPI CtrlHandler(DWORD) {
    g_stop = true;
    return TRUE;
}

void MustAddMethod(const char* name, oa::RpcFn fn, uint32_t flags) {
    if (!melange::oasis::AddMethod(name, fn, nullptr, flags)) fwprintf(stderr, L"oasis: could not register method %hs\n", name);
}
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_gameDir = ExeDir();
    std::wstring webRoot;
    bool noOpen = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--web-root" && i + 1 < argc) webRoot = Widen(argv[++i]);
        else if (std::string(argv[i]) == "--no-open") noOpen = true;
    }

    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);

    std::unique_ptr<oc::Files> files;
    std::string build = "standalone-dev";
    if (!webRoot.empty()) {
        files = oc::DirFiles(webRoot);
        std::string b;
        if (ReadWhole(webRoot + L"\\build.txt", &b)) {
            while (!b.empty() && (b.back() == '\n' || b.back() == '\r')) b.pop_back();
            build = b;
        }
    } else {
        HMODULE self = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(self, L"OASIS_WEB", MAKEINTRESOURCEW(10));
        HGLOBAL g = res ? LoadResource(self, res) : nullptr;
        const void* data = g ? LockResource(g) : nullptr;
        const size_t size = res ? SizeofResource(self, res) : 0;
        files = oc::ZipFiles(data, size);
        build = oc::ZipEntryText(data, size, "build.txt");
        if (build.empty()) build = "standalone";
    }

    oc::Host host;
    host.server = "standalone";
    oc::SetHost(host);
    oc::SetBuild(build);

    melange::oasis::providers::InstallWebPanels();
    MustAddMethod("log.sessions", &LogSessions, oa::kRpcServerThread);
    oc::AddRoute("/logs/", &RouteLogs, nullptr);
    MustAddMethod("capture.list", &CaptureList, oa::kRpcServerThread);
    oc::AddRoute("/captures/", &RouteCaptures, nullptr);
    MustAddMethod("wormsign.library", &WormsignLibrary, oa::kRpcServerThread);
    oc::AddRoute("/replays/", &RouteReplays, nullptr);
    MustAddMethod("mods.list", &ModsList, oa::kRpcServerThread);
    MustAddMethod("mods.setEnabled", &ModsSetEnabled, oa::kRpcServerThread | oa::kRpcMutating);
    MustAddMethod("ini.get", &IniGetMethod, oa::kRpcServerThread);
    MustAddMethod("ini.set", &IniSetMethod, oa::kRpcServerThread | oa::kRpcMutating);

    oc::Config cfg;
    if (!oc::Start(cfg, melange::oasis::providers::MakeAuth(), files.get())) {
        fwprintf(stderr, L"oasis: could not start the server (every port %d..%d is taken?)\n", cfg.port, cfg.port + cfg.portRange - 1);
        return 1;
    }
    const std::string url = oc::LaunchUrl();
    printf("oasis: listening, %s\n", url.c_str());
    if (!noOpen) ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    SetConsoleCtrlHandler(&CtrlHandler, TRUE);
    auto lastActive = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        oc::Pump();
        if (melange::oasis::Clients() > 0) lastActive = std::chrono::steady_clock::now();
        else if (std::chrono::steady_clock::now() - lastActive > std::chrono::minutes(10)) break;
        Sleep(200);
    }
    oc::Stop();
    return 0;
}
