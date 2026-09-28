// ini.get / ini.set: Melange.ini as text plus every declared key, and single-key edits that keep every other byte.
#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/config_schema.h"
#include "core/log.h"
#include "oasis/providers.h"
#include "oasis/rpc/ini_edit.h"
#include "oasis/rpc/ini_rpc.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"

namespace melange::oasis {
namespace {
constexpr DWORD kMaxFile = 1 << 20;
constexpr const char* kMasked = "********";
std::mutex g_writeMx;

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// False on an I/O error; a missing file reads as empty.
bool ReadFileBytes(const std::wstring& path, std::string* out, std::string* why) {
    out->clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        return *why = "cannot open Melange.ini (error " + std::to_string(GetLastError()) + ")", false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart <= kMaxFile;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out->empty() || (ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    }
    CloseHandle(f);
    if (!ok) *why = "cannot read Melange.ini (missing, locked or over 1 MB)";
    return ok;
}

bool WriteAtomic(const std::wstring& path, const std::string& bytes, std::string* why) {
    const std::wstring tmp = path + L".oasis-tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return *why = "cannot write next to Melange.ini (error " + std::to_string(GetLastError()) + ")", false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) && wrote == bytes.size();
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (ok) {
        const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        ok = exists ? ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                                   nullptr, nullptr) != 0
                    : false;
        if (!ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!ok) {
        *why = "cannot replace Melange.ini (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(tmp.c_str());
        return false;
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

bool Secret(const std::string& section, const std::string& key) {
    return _stricmp(section.c_str(), "Thumper") == 0 && _stricmp(key.c_str(), "GrantSalt") == 0;
}

bool IsLive(const std::vector<config::schema::Key>& schema, std::string_view section, std::string_view key) {
    if (section.size() > 4 && _strnicmp(section.data(), "Mod.", 4) == 0) return true;
    for (const auto& k : schema)
        if (_stricmp(k.section.c_str(), std::string(section).c_str()) == 0 && _stricmp(k.key.c_str(), std::string(key).c_str()) == 0)
            return k.live;
    return false;
}

const config::schema::Key* Declared(const std::vector<config::schema::Key>& schema, std::string_view section, std::string_view key) {
    for (const auto& k : schema)
        if (_stricmp(k.section.c_str(), std::string(section).c_str()) == 0 && _stricmp(k.key.c_str(), std::string(key).c_str()) == 0)
            return &k;
    return nullptr;
}

const char* EncodingName(ini::Encoding e) {
    return e == ini::Encoding::Utf16Le ? "utf-16le" : e == ini::Encoding::Utf8Bom ? "utf-8" : "ansi";
}
}  // namespace

namespace rpc {
void IniGet(const Call&, Result& r, void*) {
    const std::wstring& path = config::Path();
    std::string bytes, why;
    if (path.empty()) return (void)Fail(r, kRefused, "no Melange.ini");
    if (!ReadFileBytes(path, &bytes, &why)) return (void)Fail(r, kRefused, why);
    ini::Encoding enc;
    std::string text = ini::Decode(bytes, &enc);
    std::vector<ini::Entry> entries = ini::Parse(text);
    for (const ini::Entry& e : entries)
        if (Secret(e.section, e.key)) text = ini::Set(text, e.section, e.key, kMasked);
    const std::vector<config::schema::Key> schema = config::schema::All();
    jsonmini::Arr keys;
    auto add = [&](const std::string& section, const std::string& key, const config::schema::Key* decl) {
        const ini::Entry* e = ini::Find(entries, section, key);
        jsonmini::Obj o;
        o.Str("section", section).Str("key", key);
        o.Raw("def", decl ? "\"" + jsonmini::Escape(decl->def) + "\"" : "null");
        o.Bool("live", IsLive(schema, section, key)).Bool("declared", decl != nullptr);
        if (e) o.Str("current", Secret(section, key) ? kMasked : e->value).Int("line", e->line);
        else o.Raw("current", "null");
        keys.Raw(o.End());
    };
    for (const auto& k : schema) add(k.section, k.key, &k);
    for (const ini::Entry& e : entries)
        if (!Declared(schema, e.section, e.key)) add(e.section, e.key, nullptr);
    r.json = jsonmini::Obj()
                 .Str("path", Utf8(path))
                 .Str("encoding", EncodingName(enc))
                 .Str("text", text)
                 .Raw("keys", keys.End())
                 .End();
}

void IniSet(const Call& c, Result& r, void*) {
    json::Value p;
    std::string section, key, value, why;
    if (!ParseParams(c, &p, r) || !Str(p, "section", &section, r) || !Str(p, "key", &key, r) || !Str(p, "value", &value, r)) return;
    if (!ini::ValidName(section, &why) || !ini::ValidName(key, &why) || !ini::ValidValue(value, &why)) return (void)Fail(r, kBadParams, why);
    if (ini::Protected(section, key, value, &why)) return (void)Fail(r, kRefused, why);
    const std::wstring& path = config::Path();
    if (path.empty()) return (void)Fail(r, kRefused, "no Melange.ini");
    const std::vector<config::schema::Key> schema = config::schema::All();
    std::lock_guard lk(g_writeMx);
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string before;
        if (!ReadFileBytes(path, &before, &why)) return (void)Fail(r, kRefused, why);
        ini::Encoding enc;
        const std::string text = ini::Decode(before, &enc);
        const std::vector<ini::Entry> entries = ini::Parse(text);
        const ini::Entry* cur = ini::Find(entries, section, key);
        const bool modSection = section.size() > 4 && _strnicmp(section.c_str(), "Mod.", 4) == 0;
        if (!cur && !Declared(schema, section, key) && !modSection)
            return (void)Fail(r, kBadParams, "[" + section + "] " + key + " is not a Melange setting");
        const bool live = IsLive(schema, section, key);
        if (cur && cur->value == value) {
            r.json = jsonmini::Obj().Bool("live", live).Bool("restart", false).Bool("changed", false).End();
            return;
        }
        std::string bytes;
        if (!ini::Encode(ini::Set(text, section, key, value), enc, &bytes))
            return (void)Fail(r, kBadParams, "the value has characters Melange.ini's encoding cannot hold");
        std::string again;
        if (!ReadFileBytes(path, &again, &why)) return (void)Fail(r, kRefused, why);
        if (again != before) continue;
        if (!WriteAtomic(path, bytes, &why)) return (void)Fail(r, kRefused, why);
        LOG_INFO("[oasis] ini.set by client %d: [%s] %s=%s", c.client, section.c_str(), key.c_str(), value.c_str());
        r.json = jsonmini::Obj().Bool("live", live).Bool("restart", !live).Bool("changed", true).End();
        return;
    }
    Fail(r, kBusy, "Melange.ini kept changing; try again");
}
}  // namespace rpc

void providers::InstallIni() {
    AddMethod("ini.get", &rpc::IniGet, nullptr, kRpcServerThread);
    AddMethod("ini.set", &rpc::IniSet, nullptr, kRpcServerThread | kRpcMutating);
}
}  // namespace melange::oasis
