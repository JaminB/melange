// Mods\thumper-state.json: enable/disable choices, load-order pins, Deep Desert grants and whether the Mods pages
// list local plugins (Melange.exe reads and writes the same keys, oasis/standalone/mods_provider.cpp). Written
// atomically (temp file + rename) so a crash mid-save never corrupts it. Falls back to
// Documents\Melange when the game folder is read-only (Program Files).
#include <windows.h>

#include <shlobj.h>

#include <string>

#include "core/log.h"
#include "melange/mods.h"
#include "mods/thumper_internal.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

#pragma comment(lib, "ole32.lib")

namespace melange::thumper {
namespace {
State g_state;
std::wstring g_path;
bool g_pathResolved = false;

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c < 128 ? c : '?'));
    return s;
}

bool DirWritable(const std::wstring& dir) {
    std::wstring probe = dir + L"\\.thumper-write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

std::wstring DocumentsFallback() {
    PWSTR docs = nullptr;
    std::wstring out = L"Melange\\thumper-state.json";  // relative, last resort if even this fails
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        std::wstring dir = std::wstring(docs) + L"\\Melange";
        CreateDirectoryW(dir.c_str(), nullptr);
        out = dir + L"\\thumper-state.json";
    }
    if (docs) CoTaskMemFree(docs);
    return out;
}

// Resolves and caches the path Save() should use, given whether the primary (mods folder) location
// looks writable right now. Load() calls this first; later Save()s reuse whatever Load() found.
std::wstring ResolvePath() {
    std::wstring modsDir = mods::ModsDir();
    std::wstring primary = modsDir + L"\\thumper-state.json";
    if (!modsDir.empty() && DirWritable(modsDir)) return primary;
    return DocumentsFallback();
}
}  // namespace

std::wstring StatePath() {
    if (!g_pathResolved) {
        g_path = ResolvePath();
        g_pathResolved = true;
    }
    return g_path;
}

State& Live() { return g_state; }

bool Load() {
    std::wstring primary = std::wstring(mods::ModsDir()) + L"\\thumper-state.json";
    json::Value v;
    json::Error e;
    bool found = json::ParseFile(primary, &v, &e);
    if (found) {
        g_path = primary;
    } else if (e.text != "cannot open the file") {
        LOG_WARN("[thumper] %s: %d:%d %s", Narrow(primary).c_str(), e.line, e.col, e.text.c_str());
    }
    if (!found) {
        std::wstring fb = DocumentsFallback();
        if (json::ParseFile(fb, &v, &e)) {
            found = true;
            g_path = fb;
        } else if (e.text != "cannot open the file") {
            LOG_WARN("[thumper] %s: %d:%d %s", Narrow(fb).c_str(), e.line, e.col, e.text.c_str());
        }
    }
    g_pathResolved = found;  // if neither exists yet, StatePath() still resolves (and caches) on first Save()
    if (!found) {
        g_state = State{};
        return true;  // no state file yet: defaults are fine (first run)
    }
    if (!v.IsObject()) return false;
    if (const json::Value* ver = v.Get("version"); ver && ver->IsInteger()) g_state.version = static_cast<int>(ver->number);
    if (const json::Value* en = v.Get("enabled"); en && en->IsObject())
        for (const auto& [k, val] : en->members)
            if (val.IsBool()) g_state.enabled[k] = val.boolean;
    if (const json::Value* pins = v.Get("pins"); pins && pins->IsArray())
        for (const json::Value& p : pins->items) {
            const json::Value *id = p.Get("id"), *before = p.Get("before"), *after = p.Get("after");
            if (!id || !id->IsString()) continue;
            PinEntry pe;
            pe.id = id->string;
            if (before && before->IsString()) pe.before = before->string;
            if (after && after->IsString()) pe.after = after->string;
            g_state.pins.push_back(std::move(pe));
        }
    if (const json::Value* dd = v.Get("deepDesert"); dd && dd->IsObject())
        for (const auto& [id, rec] : dd->members) {
            if (!rec.IsObject()) continue;
            DeepDesertRecord r;
            if (const json::Value* g = rec.Get("granted"); g && g->IsBool()) r.granted = g->boolean;
            if (const json::Value* h = rec.Get("grantHash"); h && h->IsString()) r.grantHash = h->string;
            if (const json::Value* a = rec.Get("author"); a && a->IsString()) r.author = a->string;
            if (const json::Value* at = rec.Get("at"); at && at->IsString()) r.at = at->string;
            g_state.deepDesert[id] = std::move(r);
        }
    if (const json::Value* m = v.Get("migratedDisabledMods"); m && m->IsBool()) g_state.migratedDisabledMods = m->boolean;
    if (const json::Value* s = v.Get("showLocal"); s && s->IsBool()) g_state.showLocal = s->boolean;
    return true;
}

bool Save() {
    std::wstring path = StatePath();
    jsonmini::Obj root;
    root.Int("version", g_state.version);
    jsonmini::Obj enabled;
    for (const auto& [id, on] : g_state.enabled) enabled.Bool(id, on);
    root.Raw("enabled", enabled.End());
    jsonmini::Arr pins;
    for (const PinEntry& p : g_state.pins) {
        jsonmini::Obj o;
        o.Str("id", p.id);
        if (!p.before.empty()) o.Str("before", p.before);
        if (!p.after.empty()) o.Str("after", p.after);
        pins.Raw(o.End());
    }
    root.Raw("pins", pins.End());
    jsonmini::Obj dd;
    for (const auto& [id, r] : g_state.deepDesert) {
        jsonmini::Obj o;
        o.Bool("granted", r.granted).Str("grantHash", r.grantHash).Str("author", r.author).Str("at", r.at);
        dd.Raw(id, o.End());
    }
    root.Raw("deepDesert", dd.End());
    root.Bool("migratedDisabledMods", g_state.migratedDisabledMods);
    root.Bool("showLocal", g_state.showLocal);
    std::string text = root.End();

    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_WARN("[thumper] could not write %s", Narrow(tmp).c_str());
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LOG_WARN("[thumper] could not save %s", Narrow(path).c_str());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}
}  // namespace melange::thumper
