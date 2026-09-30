#include "erg/project.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "erg/install.h"
#include "erg/jsonio.h"
#include "levels/manifest.h"
#include "xom/json.h"

namespace melange::erg::project {
namespace {
bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

std::string Iso(const FILETIME& ft) {
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) return {};
    char b[32];
    snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}
}  // namespace

bool ValidId(std::string_view id) {
    if (id.empty() || id.size() > 24) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

std::string NowIso() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return Iso(ft);
}

Store::~Store() {
    for (auto& [id, h] : locks_) CloseHandle(static_cast<HANDLE>(h));
}

std::wstring Store::Path(const std::string& id, const char* file) const {
    return dir_ + L"\\" + install::Widen(id) + (file ? L"\\" + install::Widen(file) : L"");
}

bool Store::Exists(const std::string& id) const {
    if (!ValidId(id)) return false;
    const DWORD a = GetFileAttributesW(Path(id, nullptr).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Store::FreeId(const std::string& hint) const {
    if (!ValidId(hint)) return {};
    if (!Exists(hint)) return hint;
    for (int n = 2; n < 10000; ++n) {
        const std::string suffix = std::to_string(n);
        const std::string id = hint.substr(0, std::min(hint.size(), 24 - suffix.size())) + suffix;
        if (!Exists(id)) return id;
    }
    return {};
}

std::vector<Info> Store::List() const {
    std::vector<Info> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir_ + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string id = install::Narrow(fd.cFileName);
        if (!ValidId(id)) continue;
        WIN32_FILE_ATTRIBUTE_DATA ad{};
        if (!GetFileAttributesExW(Path(id, kPatchFile).c_str(), GetFileExInfoStandard, &ad)) continue;
        Info i;
        i.id = id;
        i.modified = Iso(ad.ftLastWriteTime);
        std::string text;
        xom::Json root;
        if (ReadPatch(id, &text, nullptr) && xom::ParseJson(text, root) && root.kind == xom::Json::Kind::Object) {
            if (const xom::Json* t = root.find("title"); t && t->kind == xom::Json::Kind::String) i.title = t->str;
            if (const xom::Json* s = root.find("stem"); s && s->kind == xom::Json::Kind::String) i.stem = s->str;
            if (const xom::Json* b = root.find("base"); b && b->kind == xom::Json::Kind::Object)
                if (const xom::Json* k = b->find("key"); k && k->kind == xom::Json::Kind::String) i.base = k->str;
        }
        Meta m;
        if (ReadMeta(id, &m)) i.built = !m.lastExport.empty() || !m.lastTest.empty();
        out.push_back(std::move(i));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.id < b.id; });
    return out;
}

bool Store::Create(const std::string& id, const std::string& patchJson, const Meta& meta, std::string* err) {
    if (!ValidId(id)) return Fail(err, "a project id is [a-z0-9]{1,24}");
    if (!install::MakeDirs(dir_)) return Fail(err, "cannot create " + install::Narrow(dir_));
    if (!CreateDirectoryW(Path(id, nullptr).c_str(), nullptr)) return Fail(err, "project '" + id + "' exists already");
    return WriteMeta(id, meta, err) && WritePatch(id, patchJson, err);
}

bool Store::ReadPatch(const std::string& id, std::string* json, std::string* err) const {
    if (!ValidId(id)) return Fail(err, "a project id is [a-z0-9]{1,24}");
    std::vector<uint8_t> b;
    if (!install::ReadFile(Path(id, kPatchFile), 4u << 20, &b, err)) return false;
    json->assign(b.begin(), b.end());
    return true;
}

bool Store::WritePatch(const std::string& id, const std::string& json, std::string* err) {
    if (!Exists(id)) return Fail(err, "no project '" + id + "'");
    return install::WriteAtomic(Path(id, kPatchFile), json.data(), json.size(), err);
}

bool Store::ReadScript(const std::string& id, std::string* text, std::string* err) const {
    text->clear();
    if (!ValidId(id)) return Fail(err, "a project id is [a-z0-9]{1,24}");
    const std::wstring path = Path(id, kScriptFile);
    if (!install::Exists(path)) return true;
    std::vector<uint8_t> b;
    if (!install::ReadFile(path, levels::manifest::kMaxSimBytes, &b, err)) return false;
    text->assign(b.begin(), b.end());
    return true;
}

bool Store::WriteScript(const std::string& id, const std::string& text, std::string* err) {
    if (!Exists(id)) return Fail(err, "no project '" + id + "'");
    const std::wstring path = Path(id, kScriptFile);
    if (!text.empty()) return install::WriteAtomic(path, text.data(), text.size(), err);
    if (!install::Exists(path) || DeleteFileW(path.c_str())) return true;
    return Fail(err, "could not remove script.lua");
}

bool Store::ReadMeta(const std::string& id, Meta* out) const {
    std::vector<uint8_t> b;
    xom::Json root;
    if (!ValidId(id) || !install::ReadFile(Path(id, "meta.json"), 64u << 10, &b, nullptr) ||
        !xom::ParseJson(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), root) ||
        root.kind != xom::Json::Kind::Object)
        return false;
    for (auto [k, dst] : {std::pair{"title", &out->title}, std::pair{"created", &out->created},
                          std::pair{"lastExport", &out->lastExport}, std::pair{"lastTest", &out->lastTest}})
        if (const xom::Json* v = root.find(k); v && v->kind == xom::Json::Kind::String) *dst = v->str;
    return true;
}

bool Store::WriteMeta(const std::string& id, const Meta& m, std::string* err) {
    if (!Exists(id)) return Fail(err, "no project '" + id + "'");
    xom::Json o = xom::Json::Obj();
    o.set("title", jsonio::Str(m.title));
    o.set("created", jsonio::Str(m.created));
    o.set("lastExport", jsonio::Str(m.lastExport));
    o.set("lastTest", jsonio::Str(m.lastTest));
    const std::string text = jsonio::Compact(o);
    return install::WriteAtomic(Path(id, "meta.json"), text.data(), text.size(), err);
}

LockResult Store::Lock(const std::string& id, uint64_t conn) {
    std::lock_guard lk(mx_);
    if (locks_.count(id)) {
        leases_[id].insert(conn);
        return LockResult::Ok;
    }
    if (!Exists(id)) return LockResult::Missing;
    HANDLE h = CreateFileW(Path(id, ".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION ? LockResult::Busy : LockResult::Failed;
    char pid[32];
    const int n = snprintf(pid, sizeof pid, "%lu", GetCurrentProcessId());
    DWORD w = 0;
    WriteFile(h, pid, static_cast<DWORD>(n), &w, nullptr);
    locks_[id] = h;
    leases_[id].insert(conn);
    return LockResult::Ok;
}

void Store::UnlockLocked(const std::string& id, uint64_t conn) {
    auto it = leases_.find(id);
    if (it == leases_.end()) return;
    it->second.erase(conn);
    if (!it->second.empty()) return;
    leases_.erase(it);
    auto lit = locks_.find(id);
    if (lit == locks_.end()) return;
    CloseHandle(static_cast<HANDLE>(lit->second));
    locks_.erase(lit);
}

void Store::Unlock(const std::string& id, uint64_t conn) {
    std::lock_guard lk(mx_);
    UnlockLocked(id, conn);
}

void Store::ReleaseConn(uint64_t conn) {
    std::lock_guard lk(mx_);
    std::vector<std::string> ids;
    for (const auto& [id, conns] : leases_)
        if (conns.count(conn)) ids.push_back(id);
    for (const auto& id : ids) UnlockLocked(id, conn);
}

bool Store::Locked(const std::string& id) const {
    std::lock_guard lk(mx_);
    return locks_.count(id) != 0;
}
}  // namespace melange::erg::project
