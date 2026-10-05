#include "store/compat.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "core/log.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::compat {
namespace {
constexpr size_t kMaxReason = 300;

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c < 128 ? c : '?'));
    return s;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string NowIso() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

bool WriteAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool SaveNotices(const std::wstring& modsDir, const std::vector<Notice>& all) {
    const std::wstring dir = QuarantineDir(modsDir);
    const std::wstring path = dir + L"\\notices.json";
    if (all.empty()) {
        DeleteFileW(path.c_str());
        RemoveDirectoryW(dir.c_str());   // only goes when nothing is quarantined either
        return true;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return WriteAtomic(path, NoticesJson(all) + "\n");
}

std::string Clip(std::string s) {
    if (s.size() > kMaxReason) s = s.substr(0, kMaxReason - 3) + "...";
    return s;
}

// Mods\.incompatible\<folder>, or <folder>-2 .. -99 when that exists: a quarantined folder is never overwritten.
std::wstring FreeSlot(const std::wstring& root, const std::wstring& folder) {
    std::wstring dest = root + L"\\" + folder;
    for (int n = 2; GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) {
        if (n > 99) return {};
        dest = root + L"\\" + folder + L"-" + std::to_wstring(n);
    }
    return dest;
}
}  // namespace

std::string Check(const std::wstring& dir, const std::string& melangeVersion, spice::Manifest* out) {
    spice::Manifest m;
    std::vector<spice::Error> errs;
    const bool parsed = spice::Parse(dir, &m, &errs);
    if (out) *out = m;
    if (!parsed) {
        std::string text;
        for (const spice::Error& e : errs) {
            if (!text.empty()) text += "; ";
            if (e.line) text += std::to_string(e.line) + ":" + std::to_string(e.col) + " ";
            text += e.text;
        }
        return Clip("its spice.json is invalid: " + (text.empty() ? std::string("unreadable") : text));
    }
    if (m.implicit) return {};
    std::string range = m.melangeRange;
    range.erase(0, range.find_first_not_of(" \t"));
    if (!range.empty() && !spice::ValidRange(range)) return Clip("its melange.range '" + m.melangeRange + "' is malformed");
    if (!spice::SemverSatisfies(melangeVersion, range))
        return Clip("needs Melange " + m.melangeRange + ", you have " + melangeVersion);
    return {};
}

std::set<std::string> StoreIds(const std::wstring& modsDir) {
    std::set<std::string> out;
    json::Value v;
    json::Error e;
    if (modsDir.empty() || !json::ParseFile(modsDir + L"\\.store\\installed.json", &v, &e) || !v.IsObject()) return out;
    for (const auto& [id, rec] : v.members)
        if (rec.IsObject() && spice::ValidModId(id)) out.insert(id);
    return out;
}

bool IsStore(const std::set<std::string>& storeIds, const std::string& id, const std::string& generatedBy) {
    return storeIds.count(id) || (!generatedBy.empty() && storeIds.count(generatedBy));
}

std::wstring QuarantineDir(const std::wstring& modsDir) { return modsDir + L"\\.incompatible"; }

std::vector<Notice> LoadNotices(const std::wstring& modsDir) {
    std::vector<Notice> out;
    json::Value v;
    json::Error e;
    if (modsDir.empty() || !json::ParseFile(QuarantineDir(modsDir) + L"\\notices.json", &v, &e) || !v.IsArray()) return out;
    for (const json::Value& x : v.items) {
        if (!x.IsObject()) continue;
        Notice n;
        auto str = [&](const char* k, std::string* s) {
            if (const json::Value* f = x.Get(k); f && f->IsString()) *s = f->string;
        };
        str("key", &n.key);
        str("id", &n.id);
        str("name", &n.name);
        str("version", &n.version);
        str("action", &n.action);
        str("reason", &n.reason);
        str("melange", &n.melange);
        str("at", &n.at);
        str("folder", &n.folder);
        str("detail", &n.detail);
        if (n.key.empty() || n.id.empty()) continue;
        out.push_back(std::move(n));
    }
    return out;
}

std::string AddNotice(const std::wstring& modsDir, Notice n) {
    std::vector<Notice> all = LoadNotices(modsDir);
    if (n.at.empty()) n.at = NowIso();
    if (n.key.empty()) {
        n.key = n.at + "-" + n.id;
        for (int k = 2; std::any_of(all.begin(), all.end(), [&](const Notice& x) { return x.key == n.key; }); ++k)
            n.key = n.at + "-" + n.id + "-" + std::to_string(k);
    }
    const std::string key = n.key;
    all.push_back(std::move(n));
    if (all.size() > kMaxNotices) all.erase(all.begin(), all.end() - static_cast<ptrdiff_t>(kMaxNotices));
    return SaveNotices(modsDir, all) ? key : std::string();
}

bool DismissNotice(const std::wstring& modsDir, const std::string& key) {
    std::vector<Notice> all = LoadNotices(modsDir);
    const size_t before = all.size();
    std::erase_if(all, [&](const Notice& n) { return key.empty() || n.key == key; });
    if (all.size() == before) return false;
    return SaveNotices(modsDir, all);
}

std::string NoticeJson(const Notice& n) {
    return jsonmini::Obj()
        .Str("key", n.key)
        .Str("id", n.id)
        .Str("name", n.name)
        .Str("version", n.version)
        .Str("action", n.action)
        .Str("reason", n.reason)
        .Str("melange", n.melange)
        .Str("at", n.at)
        .Str("folder", n.folder)
        .Str("detail", n.detail)
        .Str("text", Text(n))
        .End();
}

std::string NoticesJson(const std::vector<Notice>& all) {
    jsonmini::Arr a;
    for (const Notice& n : all) a.Raw(NoticeJson(n));
    return a.End();
}

std::string Text(const Notice& n) {
    const std::string who = n.name.empty() ? n.id : n.name;
    if (n.action == "quarantined") return "Moved " + who + " to Mods\\" + n.folder + ": " + n.reason;
    if (n.action == "removed")
        return "Removed " + who + (n.detail.empty() ? "" : " " + n.detail) + " (from the Store, no version of it can load): " + n.reason;
    if (n.action == "updated") return "Updated " + who + " to " + n.detail + ": " + n.reason;
    return who + " cannot load (" + n.reason + ")" + (n.detail.empty() ? "" : ": " + n.detail);
}

Report Sweep(const SweepContext& c) {
    Report r;
    if (c.modsDir.empty() || !IsDir(c.modsDir)) return r;
    const std::set<std::string> store = StoreIds(c.modsDir);
    std::vector<std::wstring> folders;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((c.modsDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        folders.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(folders.begin(), folders.end());

    for (const std::wstring& folder : folders) {
        const std::wstring dir = c.modsDir + L"\\" + folder;
        spice::Manifest m;
        const std::string why = Check(dir, c.melangeVersion, &m);
        if (why.empty()) continue;
        const std::string id = m.id.empty() ? Lower(Narrow(folder)) : m.id;
        // A map pack a Store importer generated counts as the Store's too (as on the Mods pages): its importer owns it.
        if (IsStore(store, id, m.generatedBy)) {
            LOG_INFO("[compat] %s (Store) cannot load: %s", id.c_str(), why.c_str());
            r.store.push_back({id, m.name, m.version, why});
            continue;
        }
        const std::wstring root = QuarantineDir(c.modsDir);
        CreateDirectoryW(root.c_str(), nullptr);
        const std::wstring dest = FreeSlot(root, folder);
        if (dest.empty() || !MoveFileExW(dir.c_str(), dest.c_str(), 0)) {
            const DWORD err = dest.empty() ? ERROR_ALREADY_EXISTS : GetLastError();
            LOG_WARN("[compat] %s cannot load (%s) but could not be moved to Mods\\.incompatible (error %lu)", id.c_str(),
                     why.c_str(), err);
            r.errors.push_back(Narrow(folder) + ": could not move it (error " + std::to_string(err) + ")");
            continue;
        }
        Notice n;
        n.id = id;
        n.name = m.name.empty() ? id : m.name;
        n.version = m.version;
        n.action = "quarantined";
        n.reason = why;
        n.melange = c.melangeVersion;
        n.at = NowIso();
        n.folder = ".incompatible\\" + Narrow(dest.substr(root.size() + 1));
        WriteAtomic(dest + L"\\.melange-quarantine.json", jsonmini::Obj()
                                                              .Str("id", n.id)
                                                              .Str("name", n.name)
                                                              .Str("version", n.version)
                                                              .Str("reason", n.reason)
                                                              .Str("melange", n.melange)
                                                              .Str("at", n.at)
                                                              .Str("from", "Mods\\" + Narrow(folder))
                                                              .End() + "\n");
        if (c.forget) c.forget(id);
        n.key = AddNotice(c.modsDir, n);
        LOG_INFO("[compat] moved %s to Mods\\%s: %s", id.c_str(), n.folder.c_str(), why.c_str());
        r.quarantined.push_back(std::move(n));
    }
    return r;
}
}  // namespace melange::compat
