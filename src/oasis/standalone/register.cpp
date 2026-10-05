#include "oasis/standalone/register.h"

#include <windows.h>

#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <mutex>

#include "melange/oasis.h"
#include "oasis/core/http.h"
#include "oasis/core/router.h"
#include "oasis/core/server.h"
#include "oasis/providers.h"
#include "oasis/rpc/ini_edit.h"
#include "oasis/standalone/ini_edit.h"
#include "oasis/standalone/level_provider.h"
#include "oasis/standalone/mods_provider.h"
#include "oasis/standalone/wormsign_provider.h"
#include "store/compat.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::oasis::standalone {
namespace oa = melange::oasis;
namespace oc = melange::oasis::core;

namespace {
StandaloneHost g_host;
constexpr char kNoGame[] = "Choose your game folder first.";

std::wstring GameDir() { return g_host.gameDir ? g_host.gameDir() : std::wstring(); }
std::string WriteGate() { return g_host.writeGate ? g_host.writeGate() : std::string(); }

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

std::wstring Documents(const wchar_t* sub) {
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = std::wstring(docs) + L"\\Melange\\" + sub;
    if (docs) CoTaskMemFree(docs);
    return out;
}

void Refuse(oa::Result& r, int code, const std::string& msg) {
    r.ok = false;
    r.code = code;
    r.message = msg;
}

// ---------------------------------------------------------------- log.sessions, /logs/
bool LooksLikeSession(const std::wstring& name) {
    if (name.size() < 20) return false;
    static const int kDigitAt[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (int i : kDigitAt)
        if (!iswdigit(name[static_cast<size_t>(i)])) return false;
    return true;
}

void LogSessions(const oa::Call&, oa::Result& r, void*) {
    const std::wstring root = LogsDir(GameDir());
    jsonmini::Arr sessions;
    WIN32_FIND_DATAW fd{};
    HANDLE h = root.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((root + L"\\*").c_str(), &fd);
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

// /logs/<session>/<file>: the session must be a real session folder under the (ini-configurable) logs directory and
// the file a plain name directly inside it; a configurable Dir must not turn this into a browser for the disk.
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
    const std::wstring root = LogsDir(GameDir());
    if (root.empty()) return false;
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
    const std::wstring game = GameDir();
    const std::string dir = IniGet(game, "MirageTrace", "CaptureDir", "");
    if (!dir.empty()) {
        const std::wstring w = Widen(dir);
        if (w.size() > 1 && w[1] == L':') return w;
        if (!game.empty()) return game + L"\\" + w;
    }
    const std::wstring docs = Documents(L"captures");
    return !docs.empty() ? docs : game.empty() ? std::wstring() : game + L"\\Melange\\captures";
}

void CaptureList(const oa::Call&, oa::Result& r, void*) {
    const std::wstring root = CapturesDir();
    jsonmini::Arr arr;
    WIN32_FIND_DATAW fd{};
    HANDLE h = root.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((root + L"\\*.mcap").c_str(), &fd);
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
    const std::wstring root = CapturesDir();
    if (root.empty()) return false;
    const std::wstring full = root + L"\\" + Widen(name);
    const DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    out->status = 200;
    out->contentType = oc::MimeType(name);
    out->file = full;
    return true;
}

// ---------------------------------------------------------------- wormsign.library, /replays/
std::wstring ReplaysDir() {
    const std::wstring docs = Documents(L"replays");
    const std::wstring game = GameDir();
    return !docs.empty() ? docs : game.empty() ? std::wstring() : game + L"\\Melange\\replays";
}

void WormsignLibrary(const oa::Call&, oa::Result& r, void*) {
    const std::wstring dir = ReplaysDir();
    r.json = dir.empty() ? "[]" : wormsignprov::ListJson(dir);
}

// /replays/<name>.wsr|.zip: flat, no subfolders. arm/control/pin/detail are not offered here (no live session).
bool RouteReplays(const oc::Request& rq, oc::Response* out, void*) {
    constexpr size_t kPrefixLen = 9;  // "/replays/"
    if (rq.path.size() <= kPrefixLen) return false;
    const std::string name = rq.path.substr(kPrefixLen);
    if (name.find('/') != std::string::npos || name.size() < 5) return false;
    const std::string ext = name.substr(name.size() - 4);
    if ((ext != ".wsr" && ext != ".zip") || !oc::SafePath(name)) return false;
    const std::wstring root = ReplaysDir();
    if (root.empty()) return false;
    const std::wstring full = root + L"\\" + Widen(name);
    const DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    out->status = 200;
    out->contentType = ext == ".zip" ? "application/zip" : "application/octet-stream";
    out->file = full;
    return true;
}

// ---------------------------------------------------------------- mods.list, mods.setEnabled
void ModsList(const oa::Call&, oa::Result& r, void*) {
    const std::wstring game = GameDir();
    if (game.empty()) return Refuse(r, -32000, kNoGame);
    r.json = modsprov::ListJson(game, g_host.version);
}

void ModsSetEnabled(const oa::Call& c, oa::Result& r, void*) {
    const std::wstring game = GameDir();
    if (game.empty()) return Refuse(r, -32000, kNoGame);
    if (const std::string gate = WriteGate(); !gate.empty()) return Refuse(r, -32000, gate);
    melange::json::Value p;
    melange::json::Error e;
    if (!melange::json::Parse(c.paramsJson, &p, &e) || !p.IsObject()) return Refuse(r, -32602, "bad params");
    const melange::json::Value *id = p.Get("id"), *on = p.Get("on");
    if (!id || !id->IsString() || !on || !on->IsBool()) return Refuse(r, -32602, "expected {id, on}");
    const int rc = modsprov::SetEnabled(game, id->string, on->boolean);
    if (rc == 0) return Refuse(r, -32602, "no such mod");
    if (rc < 0) return Refuse(r, -32000, "could not write thumper-state.json");
    r.json = "true";
}

// ---------------------------------------------------------------- mods.view, mods.setShowLocal, mods.dismissNotice
void ModsView(const oa::Call&, oa::Result& r, void*) {
    const std::wstring game = GameDir();
    if (game.empty()) return Refuse(r, -32000, kNoGame);
    r.json = modsprov::ViewJson(game);
}

void ModsSetShowLocal(const oa::Call& c, oa::Result& r, void*) {
    const std::wstring game = GameDir();
    if (game.empty()) return Refuse(r, -32000, kNoGame);
    if (const std::string gate = WriteGate(); !gate.empty()) return Refuse(r, -32000, gate);
    melange::json::Value p;
    melange::json::Error e;
    if (!melange::json::Parse(c.paramsJson, &p, &e) || !p.IsObject()) return Refuse(r, -32602, "bad params");
    const melange::json::Value* on = p.Get("on");
    if (!on || !on->IsBool()) return Refuse(r, -32602, "expected {on}");
    if (!modsprov::SetShowLocal(game, on->boolean)) return Refuse(r, -32000, "could not write thumper-state.json");
    r.json = modsprov::ViewJson(game);
}

void ModsDismissNotice(const oa::Call& c, oa::Result& r, void*) {
    const std::wstring game = GameDir();
    if (game.empty()) return Refuse(r, -32000, kNoGame);
    // The running game writes notices.json too (its own sweep, its Mods page): no read-modify-write beside it.
    if (const std::string gate = WriteGate(); !gate.empty()) return Refuse(r, -32000, gate);
    melange::json::Value p;
    melange::json::Error e;
    if (!melange::json::Parse(c.paramsJson.empty() ? std::string("{}") : c.paramsJson, &p, &e) || !p.IsObject())
        return Refuse(r, -32602, "bad params");
    const melange::json::Value* key = p.Get("key");
    if (key && !key->IsString()) return Refuse(r, -32602, "key must be a string");
    melange::compat::DismissNotice(game + L"\\Mods", key ? key->string : std::string());
    r.json = modsprov::ViewJson(game);
}

// ---------------------------------------------------------------- ini.get, ini.set
void IniGetMethod(const oa::Call&, oa::Result& r, void*) {
    const std::wstring game = GameDir();
    if (game.empty()) return Refuse(r, -32000, kNoGame);
    const std::wstring path = game + L"\\Melange.ini";
    std::string text;
    ReadWhole(path, &text);
    auto secret = [](const auto& e) { return _stricmp(e.section.c_str(), "Thumper") == 0 && _stricmp(e.key.c_str(), "GrantSalt") == 0; };
    const auto entries = melange::oasis::ini::Parse(text);
    for (const auto& e : entries)
        if (secret(e)) text = melange::oasis::ini::Set(text, e.section, e.key, "********");
    // The game isn't running, so there is no module schema: the shipped Melange.ini supplies the keys and defaults.
    std::string defText;
    const std::wstring defPath = g_host.defaultsIni ? g_host.defaultsIni() : std::wstring();
    if (!defPath.empty() && _wcsicmp(defPath.c_str(), path.c_str()) != 0) ReadWhole(defPath, &defText);
    const auto defs = melange::oasis::ini::Parse(defText.empty() ? text : defText);
    jsonmini::Arr keys;
    auto add = [&](const melange::oasis::ini::Entry* d, const melange::oasis::ini::Entry& k) {
        const auto* e = melange::oasis::ini::Find(entries, k.section, k.key);
        jsonmini::Obj o;
        o.Str("section", k.section).Str("key", k.key);
        if (d) o.Str("def", secret(*d) ? "" : d->value);
        else o.Raw("def", "null");
        o.Bool("live", false).Bool("declared", d != nullptr);
        if (e) o.Str("current", secret(*e) ? "********" : e->value).Int("line", e->line);
        else o.Raw("current", "null");
        keys.Raw(o.End());
    };
    for (const auto& d : defs) add(&d, d);
    for (const auto& e : entries)
        if (!melange::oasis::ini::Find(defs, e.section, e.key)) add(nullptr, e);
    r.json = jsonmini::Obj().Str("path", Narrow(path)).Str("text", text).Raw("keys", keys.End()).End();
}

void IniSetMethod(const oa::Call& c, oa::Result& r, void*) {
    const std::wstring game = GameDir();
    if (game.empty()) return Refuse(r, -32000, kNoGame);
    if (const std::string gate = WriteGate(); !gate.empty()) return Refuse(r, -32000, gate);
    melange::json::Value p;
    melange::json::Error e;
    if (!melange::json::Parse(c.paramsJson, &p, &e) || !p.IsObject()) return Refuse(r, -32602, "bad params");
    const melange::json::Value *section = p.Get("section"), *key = p.Get("key"), *value = p.Get("value");
    if (!section || !section->IsString() || !key || !key->IsString() || !value || !value->IsString())
        return Refuse(r, -32602, "expected {section, key, value}");
    if (std::string why; !melange::oasis::ini::ValidName(section->string, &why) || !melange::oasis::ini::ValidName(key->string, &why) ||
                          !melange::oasis::ini::ValidValue(value->string, &why))
        return Refuse(r, -32602, why);
    if (std::string why; melange::oasis::ini::Protected(section->string, key->string, value->string, &why)) return Refuse(r, -32000, why);
    const std::wstring path = game + L"\\Melange.ini";
    std::string text;
    ReadWhole(path, &text);
    std::string out;
    if (!ini::Set(text, section->string, key->string, value->string, &out)) return Refuse(r, -32602, "value cannot contain a line break or ';'");
    FILE* f = _wfopen((path + L".tmp").c_str(), L"wb");
    bool ok = f != nullptr;
    if (f) {
        ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
    }
    if (ok) ok = MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) {
        DeleteFileW((path + L".tmp").c_str());
        return Refuse(r, -32000, "could not write Melange.ini");
    }
    r.json = jsonmini::Obj().Bool("live", false).Bool("restart", true).End();
}

void MustAddMethod(const char* name, oa::RpcFn fn, uint32_t flags) {
    if (!melange::oasis::AddMethod(name, fn, nullptr, flags)) fwprintf(stderr, L"oasis: could not register method %hs\n", name);
}
}  // namespace

std::string IniGet(const std::wstring& gameDir, const char* section, const char* key, const char* def) {
    std::string text;
    if (gameDir.empty() || !ReadWhole(gameDir + L"\\Melange.ini", &text)) return def;
    const std::string v = ini::Get(text, section, key);
    return v.empty() ? def : v;
}

std::wstring LogsDir(const std::wstring& gameDir) {
    const std::string dir = IniGet(gameDir, "Logging", "Dir", "");
    if (!dir.empty()) {
        const std::wstring w = Widen(dir);
        if (w.size() > 1 && w[1] == L':') return w;
        if (!gameDir.empty()) return gameDir + L"\\" + w;
    }
    std::wstring out = Documents(L"logs");
    if ((out.empty() || GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES) && !gameDir.empty()) out = gameDir + L"\\Melange\\logs";
    return out;
}

void RegisterStandalone(const StandaloneHost& host) {
    g_host = host;
    melange::oasis::providers::InstallWebPanels();
    MustAddMethod("log.sessions", &LogSessions, oa::kRpcServerThread);
    oc::AddRoute("/logs/", &RouteLogs, nullptr);
    MustAddMethod("capture.list", &CaptureList, oa::kRpcServerThread);
    oc::AddRoute("/captures/", &RouteCaptures, nullptr);
    MustAddMethod("wormsign.library", &WormsignLibrary, oa::kRpcServerThread);
    oc::AddRoute("/replays/", &RouteReplays, nullptr);
    MustAddMethod("mods.list", &ModsList, oa::kRpcServerThread);
    MustAddMethod("mods.setEnabled", &ModsSetEnabled, oa::kRpcServerThread | oa::kRpcMutating);
    MustAddMethod("mods.view", &ModsView, oa::kRpcServerThread);
    MustAddMethod("mods.setShowLocal", &ModsSetShowLocal, oa::kRpcServerThread | oa::kRpcMutating);
    MustAddMethod("mods.dismissNotice", &ModsDismissNotice, oa::kRpcServerThread | oa::kRpcMutating);
    MustAddMethod("ini.get", &IniGetMethod, oa::kRpcServerThread);
    MustAddMethod("ini.set", &IniSetMethod, oa::kRpcServerThread | oa::kRpcMutating);
}

void RegisterLevels(const std::wstring& gameDir) {
    static std::once_flag once;
    std::call_once(once, [&] {
        if (IniGet(gameDir, "Erg", "Enabled", "1") == "0") return;
        std::wstring projects = Widen(IniGet(gameDir, "Erg", "ProjectsDir", ""));
        if (!projects.empty() && !(projects.size() > 1 && projects[1] == L':')) projects = gameDir + L"\\" + projects;
        levelprov::Install(gameDir, projects, g_host.version);
        melange::oasis::providers::InstallErgAssetRoute(gameDir, atoi(IniGet(gameDir, "Erg", "PreviewCacheMB", "512").c_str()));
    });
}
}  // namespace melange::oasis::standalone
