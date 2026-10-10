#include "launcher/setup/vanilla.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>

#include "core/log.h"
#include "launcher/setup/laa.h"
#include "launcher/util.h"
#include "tools/hash.h"
#include "tools/json_mini.h"

namespace melange::launcher::setup {
namespace {
// Every name a DLL-proxy loader or post-processing injector can take beside WormsMayhem.exe.
const wchar_t* const kLoaderDlls[] = {L"dinput8.dll", L"dsound.dll", L"winmm.dll", L"version.dll", L"d3d9.dll", L"xinput1_3.dll",
                                      L"winhttp.dll", L"wininet.dll", L"opengl32.dll"};
// Files the game (or Steam) writes into its own folder: settings, engine shadow caches and logs.
const wchar_t* const kKeepRoot[] = {L"local.cfg", L"default.cfg", L"steam_appid.txt", L"user.cfg"};
constexpr size_t kMaxEntries = 200000;   // a folder with more than this is not a game folder

std::string N(const std::wstring& s) { return Narrow(s); }

bool StartsWith(const std::wstring& s, const wchar_t* p) { return s.rfind(p, 0) == 0; }
bool EndsWith(const std::wstring& s, const wchar_t* p) {
    const size_t n = wcslen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

struct Entry {
    std::wstring rel;   // as on disk
    uint64_t size = 0;
    bool dir = false, link = false;   // link: a junction or symlinked folder (deleted as itself, never entered)
};

// Every file and folder under `g`, depth first. False when there are too many.
bool Walk(const std::wstring& g, const std::wstring& rel, std::vector<Entry>* out) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g + L"\\" + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (out->size() >= kMaxEntries) {
            ok = false;
            break;
        }
        Entry e;
        e.rel = rel + n;
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.link = e.dir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out->push_back(e);
        if (e.dir && !e.link) ok = Walk(g, e.rel + L"\\", out);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

std::wstring NameOf(const std::wstring& k) {
    const size_t s = k.rfind(L'\\');
    return s == std::wstring::npos ? k : k.substr(s + 1);
}
std::wstring DirOf(const std::wstring& k) {
    const size_t s = k.rfind(L'\\');
    return s == std::wstring::npos ? std::wstring() : k.substr(0, s);
}
std::wstring TopOf(const std::wstring& k) {
    const size_t s = k.find(L'\\');
    return s == std::wstring::npos ? std::wstring() : k.substr(0, s);
}

// k: lower-case relative path of a file that isn't stock.
bool Keep(const std::wstring& k) {
    const std::wstring name = NameOf(k), dir = DirOf(k), top = TopOf(k);
    if (dir.empty()) {
        for (const wchar_t* f : kKeepRoot)
            if (name == f) return true;
        if (StartsWith(name, L"xom") && name.find(L'-') != std::wstring::npos && EndsWith(name, L".log")) return true;   // XOM*-*.log
        if (StartsWith(name, L"net_") && EndsWith(name, L".log")) return true;                                        // Net_*.log
        if (EndsWith(name, L".sav")) return true;
        // A GOG install's own files (the stock list is Steam's): Galaxy's metadata and runtime, the uninstaller.
        if (StartsWith(name, L"goggame-") || StartsWith(name, L"unins0") || (StartsWith(name, L"galaxy") && EndsWith(name, L".dll")) ||
            name == L"gog.ico" || name == L"support.ico" || name == L"webcache.zip")
            return true;
    }
    if ((dir.empty() || top == L"data") && EndsWith(name, L".csh")) return true;   // the engine's shadow caches
    if (top == L"redist") return true;                                             // Steam redistributables
    if (top == L"__redist" || top == L"__support") return true;                    // GOG's redistributables and support files
    // Saves live in Steam\userdata\<id>\70600\remote, outside the folder; this is a safety net for anything else.
    return top == L"save" || top == L"saves" || top == L"savedata" || top == L"savegames";
}

bool IsReplay(const std::wstring& k) {
    const std::wstring name = NameOf(k);
    return EndsWith(name, L".wsr") || (StartsWith(name, L"desync-") && EndsWith(name, L".zip"));
}

bool InAsiDir(const std::wstring& dir) { return dir.empty() || dir == L"scripts" || dir == L"plugins"; }

std::string FirstLine(const std::wstring& path) {
    std::string text;
    if (!ReadAll(path, &text, 4096)) return {};
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    const size_t nl = text.find_first_of("\r\n");
    if (nl != std::string::npos) text.resize(nl);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.erase(text.begin());
    for (char& ch : text)
        if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    if (text.size() > 60) text.resize(60);
    return text;
}

struct Detected {
    bool renewation = false, wumpatch = false;
    std::string renewationLabel = "Renewation HD";
};

Detected Detect(const std::wstring& g) {
    Detected d;
    const std::string first = FirstLine(g + L"\\Version.txt");
    if (first.size() >= 10 && IEquals(first.substr(0, 10), "Renewation")) {
        d.renewation = true;
        d.renewationLabel = first;
    } else if (FileExists(g + L"\\plugins\\patch.asi") && (FileExists(g + L"\\plugins\\patch.ini") || FileExists(g + L"\\plugins\\patch.ini.bak"))) {
        d.renewation = true;
    }
    d.wumpatch = FileExists(g + L"\\plugins\\WUM.Patch.asi") || FileExists(g + L"\\plugins\\WUM.Loader.asi") || DirExists(g + L"\\Data2");
    return d;
}

struct Groups {
    std::vector<VanillaGroup> list;
    void Add(const std::string& id, const std::string& label) {
        for (auto& x : list)
            if (x.id == id && (id != "loader" || x.label == label)) {
                ++x.files;
                if (id == "reshade" || id == "specialk") {
                    if (label.size() > x.label.size()) x.label = label;   // the DLL's versioned name wins
                }
                return;
            }
        list.push_back(VanillaGroup{id, label, 1});
    }
};

// Which framework a non-stock file belongs to. k: lower-case relative path.
void Attribute(const std::wstring& g, const std::wstring& rel, const std::wstring& k, const Detected& d, Groups* groups) {
    const std::wstring name = NameOf(k), dir = DirOf(k), top = TopOf(k);
    // Melange, and its old names.
    if ((InAsiDir(dir) && (name == L"melange.asi" || name == L"melange.asi.off" || name == L"wumfix.asi" || name == L"wumfix.ini")) ||
        (dir.empty() && (name == L"melange.ini" || name == L"melange.exe" || name == L"melange.exe.old" || name == L"oasis.exe")) ||
        top == L"melange" || top == L"mods" || StartsWith(k, L"scripts\\melange\\") || StartsWith(k, L"plugins\\melange\\"))
        return groups->Add("melange", "Melange");
    // WUMPatch: its loader and patch, Data2\, and the Chinese/Korean language files.
    if ((dir == L"plugins" && StartsWith(name, L"wum.")) || top == L"data2" ||
        (dir == L"data\\language\\pc" && (StartsWith(name, L"chi") || StartsWith(name, L"kor")) && EndsWith(name, L".xom")))
        return groups->Add("wumpatch", "WUMPatch");
    // Renewation HD (with its MMP map pack): its patch, its credits and links, and every extra file under Data\.
    if (d.renewation &&
        ((dir.empty() && (name == L"version.txt" || name == L"credits.txt" || name == L"logo.png" || EndsWith(name, L".url"))) ||
         (dir == L"plugins" && StartsWith(name, L"patch.")) || top == L"data"))
        return groups->Add("renewation", d.renewationLabel);
    if (StartsWith(name, L"worms4uhd.mousefix")) return groups->Add("mousefix", "Worms4UHD MouseFix");
    if ((dir.empty() && StartsWith(name, L"reshade")) || top == L"reshade-shaders") return groups->Add("reshade", "ReShade");
    if ((dir.empty() && StartsWith(name, L"specialk")) || top == L"specialk") return groups->Add("specialk", "Special K");
    if (dir.empty() && StartsWith(name, L"dgvoodoo")) return groups->Add("dgvoodoo", "dgVoodoo");
    if (dir.empty())
        for (const wchar_t* l : kLoaderDlls)
            if (name == l) {
                DllInfo info;
                std::string label;
                if (IdentifyDll(g + L"\\" + rel, "", &info)) label = Describe(info);
                if (info.kind == DllKind::ReShade) return groups->Add("reshade", label.empty() ? "ReShade" : label);
                if (info.kind == DllKind::SpecialK) return groups->Add("specialk", label.empty() ? "Special K" : label);
                return groups->Add("loader", label.empty() ? N(rel) : label + " (" + N(rel) + ")");
            }
    if (InAsiDir(dir) && EndsWith(name, L".asi")) return groups->Add("asi", "ASI plugins");
    groups->Add("other", "Other files");
}

std::string PlanKey(const VanillaPlan& p) {
    std::string k;
    for (const auto& r : p.remove) k += "r" + r + "\x1e";
    for (const auto& r : p.replays) k += "m" + r + "\x1e";
    for (const auto& r : p.modified) k += "x" + r + "\x1e";
    for (const auto& r : p.missing) k += "-" + r + "\x1e";
    return hashutil::Sha256Hex(k.data(), k.size()).substr(0, 16);
}

std::wstring DocumentsReplays() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) out = std::wstring(p) + L"\\Melange\\replays";
    if (p) CoTaskMemFree(p);
    return out;
}

// `dir`\`name`, or "name (2).ext", "name (3).ext"... when taken: never overwrites.
std::wstring FreeName(const std::wstring& dir, const std::wstring& name) {
    std::wstring path = dir + L"\\" + name;
    const size_t dot = name.rfind(L'.');
    const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot), ext = dot == std::wstring::npos ? L"" : name.substr(dot);
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) path = dir + L"\\" + stem + L" (" + std::to_wstring(n) + L")" + ext;
    return path;
}

bool AccessDenied(unsigned long e) { return e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD || e == ERROR_WRITE_PROTECT; }

unsigned long DefaultRemove(const std::wstring& path, bool dir) {
    if (dir) return RemoveDirectoryW(path.c_str()) ? 0 : GetLastError();
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str()) ? 0 : GetLastError();
}

VanillaOutcome Refuse(int code, const std::string& msg, const std::wstring& path = {}, unsigned long win32 = 0) {
    VanillaOutcome o;
    o.outcome.code = code;
    o.outcome.message = msg;
    o.outcome.failedPath = N(path);
    o.outcome.win32 = win32;
    return o;
}

void Arr(jsonmini::Obj& o, const char* key, const std::vector<std::string>& v, size_t cap = 500) {
    jsonmini::Arr a;
    for (size_t i = 0; i < v.size() && i < cap; ++i) a.Str(v[i]);
    o.Raw(key, a.End());
}
}  // namespace

bool ParseStockList(std::string_view tsv, StockList* out, std::string* err) {
    *out = StockList{};
    size_t line = 0, i = 0;
    while (i < tsv.size()) {
        size_t nl = tsv.find('\n', i);
        if (nl == std::string_view::npos) nl = tsv.size();
        std::string_view l = tsv.substr(i, nl - i);
        i = nl + 1;
        ++line;
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        if (l.empty() || l[0] == '#') continue;
        const size_t tab = l.find('\t');
        if (tab == std::string_view::npos || tab == 0) return *err = "line " + std::to_string(line) + ": expected path<TAB>size", false;
        const std::string path(l.substr(0, tab));
        const std::string_view size = l.substr(tab + 1);
        if (size.empty() || size.size() > 15 || size.find_first_not_of("0123456789") != std::string_view::npos)
            return *err = "line " + std::to_string(line) + ": bad size", false;
        if (path.find("..") != std::string::npos || path.find(':') != std::string::npos || path.find('/') != std::string::npos ||
            path[0] == '\\' || path.back() == '\\')
            return *err = "line " + std::to_string(line) + ": bad path", false;
        const std::wstring k = LowerW(Widen(path));
        out->files[k] = std::stoull(std::string(size));
        for (std::wstring d = DirOf(k); !d.empty(); d = DirOf(d)) out->dirs.insert(d);
    }
    if (out->files.empty()) return *err = "empty", false;
    return true;
}

const StockList& EmbeddedStockList() {
    static const StockList list = [] {
        StockList l;
        HMODULE self = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(self, L"WUM_STOCK", MAKEINTRESOURCEW(10));
        HGLOBAL h = res ? LoadResource(self, res) : nullptr;
        const char* data = h ? static_cast<const char*>(LockResource(h)) : nullptr;
        std::string err;
        if (data && !ParseStockList(std::string_view(data, SizeofResource(self, res)), &l, &err)) {
            LOG_WARN("[vanilla] the embedded stock list is unreadable: %s", err.c_str());
            l = StockList{};
        }
        return l;
    }();
    return list;
}

VanillaPlan MakeVanillaPlan(const VanillaContext& ctx) {
    VanillaPlan p;
    const Context& c = ctx.base;
    const std::wstring g = c.gameDir;
    const StockList& stock = ctx.stock ? *ctx.stock : EmbeddedStockList();
    p.replaysDir = !ctx.replaysDir.empty() ? ctx.replaysDir : DocumentsReplays();
    p.store = c.storeOf && !g.empty() ? c.storeOf(g) : "unknown";
    auto refuse = [&](const std::string& why) {
        p.refused = why;
        p.code = -32000;
        p.planId = PlanKey(p);
        return p;
    };
    if (const std::string gate = GameGate(c); !gate.empty()) return refuse(gate);
    if (stock.files.find(L"wormsmayhem.exe") == stock.files.end()) return refuse("This copy of Melange.exe has no list of the game's files. Download Melange again.");
    if (FullPath(g).size() <= 3) return refuse("Restore vanilla only works on the game's own folder, not a whole drive.");
    if (p.replaysDir.empty()) return refuse("Windows didn't tell us where your Documents folder is, so there's nowhere safe to keep your replays.");
    if (PathInside(p.replaysDir, g)) return refuse("Your Documents folder is inside the game folder. Move it first.");

    std::vector<Entry> all;
    if (!Walk(g, L"", &all)) return refuse("This folder holds far more files than the game. Check that it is the right folder.");
    const Detected d = Detect(g);
    Groups groups;
    std::unordered_set<std::wstring> present;
    for (const auto& e : all) {
        const std::wstring k = LowerW(e.rel);
        if (e.dir && !e.link) continue;
        present.insert(k);
        if (const auto it = stock.files.find(k); it != stock.files.end() && !e.dir) {
            if (it->second != 0 && it->second != e.size) p.modified.push_back(N(e.rel));
            continue;
        }
        if (!e.link && Keep(k)) continue;
        if (!e.link && IsReplay(k)) {
            p.replays.push_back(N(e.rel));
            continue;
        }
        p.remove.push_back(N(e.rel));
        p.removeBytes += e.link ? 0 : e.size;
        Attribute(g, e.rel, k, d, &groups);
        if (!c.selfExe.empty() && PathKey(c.selfExe) == PathKey(g + L"\\" + e.rel)) p.selfInGame = true;
    }
    for (const auto& [k, size] : stock.files)
        if (!present.count(k)) p.missing.push_back(N(k));
    std::sort(p.missing.begin(), p.missing.end());
    // Display order: the named frameworks first, loose plugins and everything else last.
    static const char* const kOrder[] = {"melange", "renewation", "wumpatch", "loader", "reshade", "specialk", "dgvoodoo", "mousefix", "asi", "other"};
    for (const char* id : kOrder)
        for (const auto& x : groups.list)
            if (x.id == id) p.groups.push_back(x);
    p.overwrites = d.renewation || d.wumpatch;
    p.verify = p.overwrites || !p.modified.empty() || !p.missing.empty();
    p.planId = PlanKey(p);
    return p;
}

std::string VanillaPlanJson(const VanillaPlan& p) {
    jsonmini::Obj o;
    o.Str("planId", p.planId);
    if (!p.refused.empty()) o.Str("refused", p.refused);
    jsonmini::Arr groups;
    for (const auto& x : p.groups) groups.Raw(jsonmini::Obj().Str("id", x.id).Str("label", x.label).Int("files", x.files).End());
    o.Raw("groups", groups.End()).Int("files", static_cast<long long>(p.remove.size())).UInt("bytes", p.removeBytes);
    std::vector<std::string> sample(p.remove.begin(), p.remove.begin() + std::min<size_t>(p.remove.size(), 200));
    Arr(o, "sample", sample);
    Arr(o, "replays", p.replays);
    o.Str("replaysDir", N(p.replaysDir));
    Arr(o, "modified", p.modified);
    o.Int("modifiedCount", static_cast<long long>(p.modified.size()));
    Arr(o, "missing", p.missing);
    o.Int("missingCount", static_cast<long long>(p.missing.size()));
    o.Bool("overwrites", p.overwrites).Bool("verify", p.verify).Str("store", p.store).Bool("selfInGame", p.selfInGame);
    return o.End();
}

VanillaOutcome ApplyVanilla(const VanillaContext& ctx, const std::string& planId) {
    const Context& c = ctx.base;
    const std::wstring g = c.gameDir;
    VanillaPlan p = MakeVanillaPlan(ctx);
    if (!p.refused.empty()) return Refuse(p.code ? p.code : -32000, p.refused);
    if (!planId.empty() && planId != p.planId) return Refuse(-32013, "The game folder changed since the plan was shown. Check the new plan and confirm again.");
    const StockList& stock = ctx.stock ? *ctx.stock : EmbeddedStockList();
    VanillaOutcome o;
    o.plan = p;
    const int of = static_cast<int>(p.replays.size() + p.remove.size()) + 1;
    int step = 0;
    auto progress = [&](const std::string& label) {
        ++step;
        if (c.progress && (step % 25 == 0 || step == of)) c.progress(step, of, label);
    };
    LOG_INFO("[vanilla] restoring %ls: %zu files to delete, %zu replays to move, %zu stock files modified, %zu missing", g.c_str(), p.remove.size(),
             p.replays.size(), p.modified.size(), p.missing.size());
    for (const auto& x : p.groups) LOG_INFO("[vanilla] found %s: %d file(s)", x.label.c_str(), x.files);

    // 1. Replays out first: if one can't be moved, nothing is deleted.
    if (!p.replays.empty() && !MakeDirs(p.replaysDir)) {
        const unsigned long e = GetLastError();
        return Refuse(-32012, "Could not create " + N(p.replaysDir) + " for your replays: " + Win32Message(e), p.replaysDir, e);
    }
    for (const auto& r : p.replays) {
        const std::wstring from = g + L"\\" + Widen(r), to = FreeName(p.replaysDir, FileName(from));
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            const unsigned long e = GetLastError();
            VanillaOutcome f = Refuse(AccessDenied(e) ? -32010 : -32012,
                                      AccessDenied(e) ? "Windows didn't let us change the game folder."
                                                      : "Nothing was deleted: could not move the replay " + r + ": " + Win32Message(e),
                                      from, e);
            f.moved = o.moved;
            f.plan = p;
            return f;
        }
        o.moved.emplace_back(r, N(to));
        LOG_INFO("[vanilla] moved %s -> %ls", r.c_str(), to.c_str());
        progress("Moved " + r);
    }

    // 1b. The 4 GB bit goes back to stock while the marker (deleted below with the rest of Melange) still says it was ours.
    if (LaaMarkerPresent(g)) {
        const LaaResult l = EnsureLaa(c, false);
        if (!l.ok) LOG_WARN("[vanilla] could not clear the 4 GB bit: %s", l.message.c_str());
    }

    // 2. Delete. This process's own files (Melange.exe run from the game folder) go once it has closed.
    for (const auto& r : p.remove) {
        const std::wstring full = g + L"\\" + Widen(r);
        if ((!c.selfExe.empty() && PathKey(c.selfExe) == PathKey(full)) || GetModuleHandleW(full.c_str())) {
            o.pending.push_back(full);
            continue;
        }
        const DWORD attrs = GetFileAttributesW(full.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) continue;
        const bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const unsigned long e = ctx.remove ? ctx.remove(full) : DefaultRemove(full, dir);
        if (e) {
            if (o.deleted == 0 && o.failed.empty() && AccessDenied(e)) {
                VanillaOutcome f = Refuse(-32010, "Windows didn't let us change the game folder.", full, e);
                f.moved = o.moved;
                f.plan = p;
                return f;
            }
            if (o.failed.empty()) {
                o.outcome.failedPath = N(full);
                o.outcome.win32 = e;
            }
            o.failed.push_back(r);
            LOG_WARN("[vanilla] could not delete %s: %s", r.c_str(), Win32Message(e).c_str());
        } else {
            ++o.deleted;
        }
        progress(r);
    }

    // 3. Folders left empty that aren't the game's own.
    std::vector<Entry> all;
    Walk(g, L"", &all);
    std::vector<std::wstring> dirs;
    for (const auto& e : all)
        if (e.dir && !e.link && !stock.dirs.count(LowerW(e.rel))) dirs.push_back(e.rel);
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });   // deepest first
    for (const auto& dd : dirs)
        if (RemoveDirectoryW((g + L"\\" + dd).c_str())) ++o.dirsRemoved;
    progress("Removed empty folders");

    LOG_INFO("[vanilla] deleted %d file(s) and %d folder(s), moved %zu replay(s); %zu failed, %zu pending until Melange closes", o.deleted,
             o.dirsRemoved, o.moved.size(), o.failed.size(), o.pending.size());
    if (!o.failed.empty()) {
        o.outcome.code = -32012;
        o.outcome.message = "Restore vanilla didn't finish: " + std::to_string(o.failed.size()) + " file" + (o.failed.size() == 1 ? "" : "s") +
                            " could not be deleted (" + o.failed.front() + ": " + Win32Message(o.outcome.win32) +
                            "). Close programs that may be using them and try again.";
        return o;
    }
    o.outcome.ok = true;
    return o;
}

std::string VanillaOutcomeJson(const VanillaOutcome& o) {
    jsonmini::Obj j;
    j.Bool("ok", o.outcome.ok).Int("deleted", o.deleted).Int("dirsRemoved", o.dirsRemoved);
    jsonmini::Arr moved;
    for (const auto& [from, to] : o.moved) moved.Raw(jsonmini::Obj().Str("from", from).Str("to", to).End());
    j.Raw("moved", moved.End()).Str("replaysDir", N(o.plan.replaysDir));
    Arr(j, "failed", o.failed);
    Arr(j, "modified", o.plan.modified);
    Arr(j, "missing", o.plan.missing);
    // The lists stop at 500 entries: the counts are the real ones.
    j.Int("modifiedCount", static_cast<long long>(o.plan.modified.size())).Int("missingCount", static_cast<long long>(o.plan.missing.size()));
    j.Bool("verify", o.plan.verify).Bool("verifyStarted", o.verifyStarted).Str("store", o.plan.store).Bool("selfPending", !o.pending.empty());
    return j.End();
}
}  // namespace melange::launcher::setup
