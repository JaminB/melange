#include "launcher/setup/engine.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "launcher/setup/ini_merge.h"
#include "launcher/setup/running.h"
#include "launcher/util.h"
#include "oasis/rpc/ini_edit.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"
#include "update/release.h"

namespace melange::launcher::setup {
namespace {
const wchar_t* const kAltLoaders[] = {L"dsound.dll", L"winmm.dll", L"version.dll", L"d3d9.dll", L"xinput1_3.dll", L"winhttp.dll",
                                      L"wininet.dll", L"opengl32.dll"};
const wchar_t* const kAsiDirs[] = {L"", L"scripts\\", L"plugins\\"};
constexpr char kProtectedCopy[] = "This folder is protected (MELANGE_PROTECT).";
constexpr char kRunningCopy[] = "Close Worms Ultimate Mayhem first.";

std::wstring W(const std::string& s) { return Widen(s); }
std::string N(const std::wstring& s) { return Narrow(s); }

bool Running(const Context& c) { return c.running ? c.running(c.gameDir) : GameRunning(c.gameDir); }
bool Loaded(const Context& c) { return c.loaded ? c.loaded(c.gameDir) : MelangeLoaded(c.gameDir); }
unsigned long Move(const Context& c, const std::wstring& a, const std::wstring& b) { return c.move ? c.move(a, b) : DefaultMove(a, b); }

// Dotted numeric compare, shared with the updater (update/release.h).
using update::CompareVersions;

bool IsProtected(const Context& c) {
    for (const auto& p : c.protect)
        if (!p.empty() && (PathInside(c.gameDir, p) || PathInside(p, c.gameDir))) return true;
    return false;
}

std::string ReadIniText(const std::wstring& path, oasis::ini::Encoding* enc) {
    std::string bytes;
    if (!ReadAll(path, &bytes, 4u << 20)) return {};
    return oasis::ini::Decode(bytes, enc);
}

std::wstring Found(const std::wstring& game, const std::wstring& name, std::vector<std::wstring>* all = nullptr) {
    std::wstring first;
    for (const wchar_t* d : kAsiDirs) {
        const std::wstring rel = std::wstring(d) + name;
        if (FileExists(game + L"\\" + rel)) {
            if (first.empty()) first = rel;
            if (all) all->push_back(rel);
        }
    }
    return first;
}

Payload ReadPayload(const Context& c) {
    Payload p;
    p.version = c.version;
    p.fromGameFolder = !c.payloadDir.empty() && PathKey(c.payloadDir) == PathKey(c.gameDir);
    for (const wchar_t* f : {L"melange.asi", L"dinput8.dll", L"Melange.ini"})
        if (c.payloadDir.empty() || !FileExists(c.payloadDir + L"\\" + f)) p.missing.push_back(N(f));
    if (p.missing.empty() || std::find(p.missing.begin(), p.missing.end(), "melange.asi") == p.missing.end()) {
        const std::wstring asi = c.payloadDir + L"\\melange.asi";
        if (FileExists(asi)) {
            const std::string v = FileProductVersion(asi);
            if (v != c.version) p.missing.push_back("melange.asi (version " + (v.empty() ? std::string("unknown") : v) + ", expected " + c.version + ")");
            else p.asiSha = Sha256Cached(asi);
        }
    }
    const std::wstring ual = c.payloadDir + L"\\dinput8.dll";
    if (!c.payloadDir.empty() && FileExists(ual)) {
        p.ualSha = Sha256Cached(ual);
        if (std::find(KnownUalHashes().begin(), KnownUalHashes().end(), p.ualSha) == KnownUalHashes().end()) {
            p.missing.push_back("dinput8.dll (not the Ultimate ASI Loader build Melange ships)");
            p.ualSha.clear();
        }
    }
    p.ok = p.missing.empty();
    return p;
}

InstallRecord ReadInstallRecord(const std::wstring& game) {
    InstallRecord r;
    json::Value v;
    json::Error e;
    if (!json::ParseFile(game + L"\\Melange\\install.json", &v, &e) || !v.IsObject()) return r;
    r.present = true;
    auto s = [&](const char* k) {
        const json::Value* x = v.Get(k);
        return x && x->IsString() ? x->string : std::string();
    };
    r.melange = s("melange");
    r.installedAt = s("installedAt");
    r.loader = s("loader");
    r.loaderSha256 = s("loaderSha256");
    return r;
}

bool SafeId(const std::string& id) {
    if (id.empty() || id.size() > 80) return false;
    bool allDots = true;
    for (char ch : id) {
        if (ch != '.') allDots = false;
        if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.')) return false;
    }
    // Not just "." or ".." (classic traversal) but any run of dots: Windows strips a trailing all-dot final
    // path component, so "..." etc. would otherwise resolve to the parent (e.g. the whole backup\ folder).
    return !allDots;
}

void LastLoad(const std::wstring& logs, std::string* at, std::string* version) {
    if (logs.empty()) return;
    std::wstring newest;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((logs + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring n = fd.cFileName;
        if (n.size() >= 19 && iswdigit(n[0]) && n[4] == L'-' && n > newest) newest = n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (newest.empty()) return;
    HANDLE f = CreateFileW((logs + L"\\" + newest + L"\\events.jsonl").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string text(4096, '\0');
    DWORD rd = 0;
    const bool ok = ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &rd, nullptr) != 0;
    CloseHandle(f);
    if (!ok) return;
    text.resize(rd);
    const size_t nl = text.find('\n');
    json::Value v;
    json::Error e;
    if (!json::Parse(std::string_view(text).substr(0, nl), &v, &e) || !v.IsObject()) return;
    if (const json::Value* w = v.Get("wall"); w && w->IsString()) *at = w->string;
    if (const json::Value* d = v.Get("data"); d && d->IsObject())
        if (const json::Value* ver = d->Get("version"); ver && ver->IsString()) *version = ver->string;
}

// ---------------------------------------------------------------- operations
enum class OpKind { Add, Replace, Remove, RemoveDir, Merge, Rename };
struct Op {
    OpKind kind;
    std::wstring rel;          // target, relative to the game folder
    std::wstring source;       // Add/Replace: the file to copy; Rename: the new relative name
    std::string text;          // Merge / Add-from-text
    bool fromText = false;
    bool backup = true;        // replaced or removed content is backed up
    std::string kindLabel, description;   // manifest extras (loader)
    std::string expectSha;     // Remove of an added file during restore: only when the hash still matches
};

struct Built {
    Plan plan;
    std::vector<Op> ops;
    std::string loaderMode;   // install record: added | reused | replaced | other-name
    std::string loaderSha;
};

std::string StepsKey(const std::vector<Step>& steps) {
    std::string k;
    for (const auto& s : steps) k += s.op + "\x1f" + s.path + "\x1f" + s.detail + "\x1e";
    return hashutil::Sha256Hex(k.data(), k.size()).substr(0, 16);
}

void AddStep(Built& b, const char* op, const std::wstring& rel, const std::string& detail) { b.plan.steps.push_back(Step{op, N(rel), detail}); }

std::string Gate(const Context& c, bool needOk, bool* exists) {
    const GameCheck g = CheckExe(c.gameDir, c.profiles ? *c.profiles : DefaultProfiles());
    if (exists) *exists = g.verdict != Verdict::NotFound;
    if (c.gameDir.empty()) return "Choose your game folder first.";
    if (g.verdict == Verdict::NotFound) return "That folder no longer exists.";
    if (needOk && g.verdict != Verdict::Ok) {
        if (g.verdict == Verdict::WrongBuild)
            return "This version of the game isn't supported. Melange only works with the Steam/GOG release, build #1077.";
        if (g.verdict == Verdict::NoExe) return "WormsMayhem.exe isn't in this folder.";
        return "We couldn't read WormsMayhem.exe." + (g.error.empty() ? std::string() : " Windows said: " + g.error);
    }
    if (IsProtected(c)) return kProtectedCopy;
    if (Running(c)) return kRunningCopy;
    return {};
}

Built BuildInstall(const Context& c, const PlanRequest& req, const Status& st) {
    Built b;
    const Payload& p = st.payload;
    const std::wstring g = c.gameDir;
    std::vector<std::string> missing;
    auto need = [&](const char* f) {
        for (const auto& m : p.missing)
            if (m.rfind(f, 0) == 0) missing.push_back(m);
    };

    for (const auto& l : st.legacy) {
        b.ops.push_back(Op{OpKind::Remove, W(l)});
        AddStep(b, "remove", W(l), l == "oasis.exe" ? "replaced by Melange.exe; a backup is kept" : "Melange's old name; a backup is kept");
    }
    for (const auto& d : st.duplicates) {
        b.ops.push_back(Op{OpKind::Remove, W(d)});
        AddStep(b, "remove", W(d), "a second copy of Melange; a backup is kept");
    }
    std::vector<std::wstring> offs;
    Found(g, L"melange.asi.off", &offs);
    for (const auto& o : offs) {
        b.ops.push_back(Op{OpKind::Remove, o});
        AddStep(b, "remove", o, "Melange was switched off; it is switched back on");
    }

    // melange.asi
    const std::wstring asiRel = st.melangePath.empty() ? L"melange.asi" : W(st.melangePath);
    const bool asiInPayload = std::find(p.missing.begin(), p.missing.end(), "melange.asi") == p.missing.end() && !p.asiSha.empty();
    const bool asiHere = st.melangeState == "installed" || st.melangeState == "older" || st.melangeState == "newer" ||
                         st.melangeState == "damaged";
    if (st.melangeState == "newer" && !req.allowDowngrade) {
        AddStep(b, "keep", asiRel, "Melange " + st.melangeVersion + " is newer than this release (" + c.version + "); kept");
    } else if (st.melangeState == "installed" && (!asiInPayload || p.fromGameFolder)) {
        AddStep(b, "keep", asiRel, "Melange " + st.melangeVersion);
    } else if (asiHere && !asiInPayload) {
        need("melange.asi");
        if (missing.empty()) missing.push_back("melange.asi");
    } else if (asiHere) {
        const bool same = hashutil::Sha256HexFile(g + L"\\" + asiRel) == p.asiSha;
        if (same) {
            AddStep(b, "keep", asiRel, "Melange " + st.melangeVersion);
        } else {
            b.ops.push_back(Op{OpKind::Replace, asiRel, c.payloadDir + L"\\melange.asi"});
            AddStep(b, "replace", asiRel,
                    st.melangeState == "older"     ? "update Melange " + st.melangeVersion + " to " + c.version
                    : st.melangeState == "damaged" ? "repair Melange " + c.version
                    : st.melangeState == "newer"   ? "go back to Melange " + c.version + "; a backup is kept"
                                                   : "replace with this release's copy of Melange " + c.version + "; a backup is kept");
        }
    } else if (!asiInPayload) {
        need("melange.asi");
        if (missing.empty()) missing.push_back("melange.asi");
    } else {
        b.ops.push_back(Op{OpKind::Add, L"melange.asi", c.payloadDir + L"\\melange.asi"});
        AddStep(b, "add", L"melange.asi", "Melange " + c.version);
    }

    // Melange.ini
    std::string templ;
    const bool haveTempl = !c.payloadDir.empty() && ReadAll(c.payloadDir + L"\\Melange.ini", &templ, 4u << 20);
    oasis::ini::Encoding tEnc{};
    if (haveTempl) templ = oasis::ini::Decode(templ, &tEnc);
    if (!st.iniPresent) {
        if (haveTempl) {
            Op o{OpKind::Add, L"Melange.ini"};
            o.source = c.payloadDir + L"\\Melange.ini";
            b.ops.push_back(o);
            AddStep(b, "add", L"Melange.ini", "the default settings");
        } else {
            need("Melange.ini");
        }
    } else if (haveTempl && !p.fromGameFolder) {
        oasis::ini::Encoding enc{};
        const std::string user = ReadIniText(g + L"\\Melange.ini", &enc);
        int added = 0;
        const std::string merged = IniMerge(templ, user, &added);
        std::string bytes;
        if (added > 0 && oasis::ini::Encode(merged, enc, &bytes)) {
            Op o{OpKind::Merge, L"Melange.ini"};
            o.text = bytes;
            o.fromText = true;
            b.ops.push_back(o);
            AddStep(b, "merge", L"Melange.ini", "keep your settings and add " + std::to_string(added) + " new one" + (added == 1 ? "" : "s") +
                                                     "; a backup is kept");
        } else {
            AddStep(b, "keep", L"Melange.ini", "your settings");
        }
    } else {
        AddStep(b, "keep", L"Melange.ini", "your settings");
    }

    // Melange.exe into the game folder
    if (!c.selfExe.empty() && FileExists(c.selfExe) && PathKey(Parent(c.selfExe)) != PathKey(g)) {
        const bool exists = FileExists(g + L"\\Melange.exe");
        const bool same = exists && hashutil::Sha256HexFile(g + L"\\Melange.exe") == hashutil::Sha256HexFile(c.selfExe);
        if (same) {
            AddStep(b, "keep", L"Melange.exe", "this app");
        } else {
            Op o{exists ? OpKind::Replace : OpKind::Add, L"Melange.exe", c.selfExe};
            o.backup = false;
            b.ops.push_back(o);
            AddStep(b, exists ? "replace" : "add", L"Melange.exe", "this app, so you can start Melange from the game folder");
        }
    }

    // The loader goes last.
    if (st.loaderState == "ual") {
        b.loaderMode = "reused";
        b.loaderSha = st.loader.sha256;
        AddStep(b, "keep", L"dinput8.dll", Describe(st.loader) + " is already installed");
    } else if (st.loaderState == "none" && !st.otherLoaders.empty()) {
        b.loaderMode = "other-name";
        b.loaderSha = st.otherLoaders.front().sha256;
        AddStep(b, "keep", W(st.otherLoaders.front().file), "an ASI loader is already installed as " + st.otherLoaders.front().file);
    } else if (p.ualSha.empty()) {
        need("dinput8.dll");
    } else if (st.loaderState == "none") {
        b.loaderMode = "added";
        b.loaderSha = p.ualSha;
        b.ops.push_back(Op{OpKind::Add, L"dinput8.dll", c.payloadDir + L"\\dinput8.dll"});
        AddStep(b, "add", L"dinput8.dll", "Ultimate ASI Loader; it lets the game load Melange");
    } else if (!req.replaceLoader) {
        b.plan.needsChoice = "loader";
        const std::string what = Describe(st.loader);
        AddStep(b, "keep", L"dinput8.dll", (what.empty() ? std::string("another program's loader") : what) + "; Melange needs Ultimate ASI Loader here");
    } else {
        b.loaderMode = "replaced";
        b.loaderSha = p.ualSha;
        Op o{OpKind::Replace, L"dinput8.dll", c.payloadDir + L"\\dinput8.dll"};
        o.kindLabel = DllKindName(st.loader.kind);
        o.description = Describe(st.loader);
        b.ops.push_back(o);
        AddStep(b, "backup", L"dinput8.dll", (o.description.empty() ? std::string("the current dinput8.dll") : o.description) + ", so you can restore it");
        AddStep(b, "replace", L"dinput8.dll", "Ultimate ASI Loader");
    }
    if (!missing.empty()) {
        b.plan.code = -32011;
        b.plan.missing = missing;
        b.plan.refused = p.fromGameFolder ? "Repair needs the release zip for missing files. Extract the whole zip you downloaded and run Melange.exe from that folder."
                                          : "Some Melange files are missing. Extract the whole zip you downloaded and run Melange.exe from that folder.";
    }
    return b;
}

Built BuildUninstall(const Context& c, const PlanRequest& req, const Status& st) {
    Built b;
    const std::wstring g = c.gameDir;
    // The loader first: put back what we replaced, remove what we added, keep anything else.
    if (FileExists(g + L"\\dinput8.dll")) {
        const InstallRecord& r = st.install;
        const Backup* from = nullptr;
        for (const auto& bk : st.backups) {   // newest first
            if (from) break;
            for (const auto& f : bk.files)
                if (IEquals(f.path, "dinput8.dll") && f.op == "replaced" && (bk.action == "install" || bk.action == "repair")) from = &bk;
        }
        if (r.loader == "replaced" && from && st.loader.sha256 == r.loaderSha256) {
            Op o{OpKind::Replace, L"dinput8.dll", g + L"\\Melange\\backup\\" + W(from->id) + L"\\dinput8.dll"};
            b.ops.push_back(o);
            std::string desc;
            for (const auto& f : from->files)
                if (IEquals(f.path, "dinput8.dll")) desc = f.description;
            AddStep(b, "replace", L"dinput8.dll", "put back " + (desc.empty() ? std::string("the previous dinput8.dll") : desc));
        } else if (r.loader == "added" && st.loader.sha256 == r.loaderSha256) {
            b.ops.push_back(Op{OpKind::Remove, L"dinput8.dll"});
            AddStep(b, "remove", L"dinput8.dll", "Ultimate ASI Loader, added by Melange");
        } else {
            AddStep(b, "keep", L"dinput8.dll", st.loaderState == "ual" ? "Ultimate ASI Loader; other mods may use it" : "not Melange's");
        }
    }
    std::vector<std::wstring> asis;
    Found(g, L"melange.asi", &asis);
    Found(g, L"melange.asi.off", &asis);
    for (const auto& a : asis) {
        b.ops.push_back(Op{OpKind::Remove, a});
        AddStep(b, "remove", a, "Melange; a backup is kept");
    }
    for (const auto& l : st.legacy) {
        b.ops.push_back(Op{OpKind::Remove, W(l)});
        AddStep(b, "remove", W(l), "an old Melange file; a backup is kept");
    }
    if (FileExists(g + L"\\Melange.exe")) {
        if (!c.selfExe.empty() && PathKey(c.selfExe) == PathKey(g + L"\\Melange.exe")) {
            AddStep(b, "keep", L"Melange.exe", "this app is running from here; delete it after closing Melange");
        } else {
            Op o{OpKind::Remove, L"Melange.exe"};
            o.backup = false;
            b.ops.push_back(o);
            AddStep(b, "remove", L"Melange.exe", "this app's copy in the game folder");
        }
    }
    // Part of the same transaction as everything else here, so a crash mid-uninstall can't leave install.json
    // behind claiming Melange is still installed once the files that back it are already gone.
    if (FileExists(g + L"\\Melange\\install.json")) {
        Op o{OpKind::Remove, L"Melange\\install.json"};
        o.backup = false;
        b.ops.push_back(o);
    }
    if (req.removeData) {
        if (st.iniPresent) {
            b.ops.push_back(Op{OpKind::Remove, L"Melange.ini"});
            AddStep(b, "remove", L"Melange.ini", "your settings; a backup is kept");
        }
        // Store-managed plugins (Mods\.store\installed.json); mods you added yourself stay.
        json::Value v;
        json::Error e;
        if (json::ParseFile(g + L"\\Mods\\.store\\installed.json", &v, &e) && v.IsObject())
            for (const auto& [id, rec] : v.members) {
                if (id.empty() || id[0] == '_' || !rec.IsObject() || !SafeId(id) || !DirExists(g + L"\\Mods\\" + W(id))) continue;
                Op o{OpKind::RemoveDir, L"Mods\\" + W(id)};
                o.backup = false;
                b.ops.push_back(o);
                AddStep(b, "remove", o.rel, "a plugin installed from the Store");
            }
        if (DirExists(g + L"\\Mods\\.store")) {
            Op o{OpKind::RemoveDir, L"Mods\\.store"};
            o.backup = false;
            b.ops.push_back(o);
            AddStep(b, "remove", o.rel, "the Store's records");
        }
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((g + L"\\Melange\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring n = fd.cFileName;
                if (n == L"." || n == L".." || _wcsicmp(n.c_str(), L"backup") == 0 || _wcsicmp(n.c_str(), L".staging") == 0 ||
                    _wcsicmp(n.c_str(), L"install.json") == 0)
                    continue;
                Op o{(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? OpKind::RemoveDir : OpKind::Remove, L"Melange\\" + n};
                o.backup = false;
                b.ops.push_back(o);
                AddStep(b, "remove", o.rel, "logs, captures and other data");
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return b;
}

Built Build(const Context& c, const PlanRequest& req, Status* stOut) {
    Status st = Inspect(c);
    Built b;
    if (req.action == "uninstall") b = BuildUninstall(c, req, st);
    else b = BuildInstall(c, req, st);
    b.plan.planId = StepsKey(b.plan.steps);
    if (b.plan.refused.empty()) {
        const std::string gate = Gate(c, req.action != "uninstall", nullptr);
        if (!gate.empty()) {
            b.plan.refused = gate;
            b.plan.code = -32000;
        }
    }
    if (stOut) *stOut = std::move(st);
    return b;
}

// ---------------------------------------------------------------- transactions
struct Done {
    enum Kind { Placed, MovedAway } kind;
    std::wstring target, aside;   // Placed: target was new (aside empty) or the old one sits at aside
};

bool CopyVerified(const std::wstring& from, const std::wstring& to, unsigned long* err) {
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        *err = GetLastError();
        return false;
    }
    SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (hashutil::Sha256HexFile(from) != hashutil::Sha256HexFile(to)) {
        *err = ERROR_CRC;
        return false;
    }
    return true;
}

bool CopyTree(const std::wstring& from, const std::wstring& to) {
    if (!MakeDirs(to)) return false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= CopyTree(from + L"\\" + n, to + L"\\" + n);
        else ok &= CopyFileW((from + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), FALSE) != 0;
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool DeleteTreeW(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                DeleteTreeW(p);
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(p.c_str());
            } else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

Outcome Fail(int code, const std::string& msg, const std::wstring& path = {}, unsigned long win32 = 0) {
    Outcome o;
    o.code = code;
    o.message = msg;
    o.failedPath = N(path);
    o.win32 = win32;
    return o;
}

bool AccessDenied(unsigned long e) { return e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD || e == ERROR_WRITE_PROTECT; }

struct ManifestFile { std::string path, op, sha256, kind, description; uint64_t size = 0; };

Outcome RunOps(const Context& c, const std::string& action, std::vector<Op> ops, std::string* backupIdOut,
               std::vector<ManifestFile>* recordOut = nullptr) {
    const std::wstring g = c.gameDir;
    const std::wstring dataDir = g + L"\\Melange";
    const std::wstring stageRoot = dataDir + L"\\.staging\\" + W(RandomHex(6));
    const int of = static_cast<int>(ops.size()) + 2;
    int step = 0;
    auto progress = [&](const std::string& label) {
        if (c.progress) c.progress(++step, of, label);
    };
    if (!MakeDirs(stageRoot)) {
        const unsigned long e = GetLastError();
        return Fail(AccessDenied(e) ? -32010 : -32012,
                    AccessDenied(e) ? "Windows didn't let us write to the game folder." : "Could not prepare the game folder: " + Win32Message(e),
                    dataDir, e);
    }
    // 1. Stage new content beside the game files (same volume: the commit is a rename).
    std::vector<std::wstring> staged(ops.size());
    for (size_t i = 0; i < ops.size(); ++i) {
        Op& o = ops[i];
        if (o.kind != OpKind::Add && o.kind != OpKind::Replace && o.kind != OpKind::Merge) continue;
        staged[i] = stageRoot + L"\\" + std::to_wstring(i);
        unsigned long e = 0;
        bool ok;
        if (o.fromText) {
            e = WriteAtomic(staged[i], o.text);
            ok = e == 0;
        } else {
            ok = CopyVerified(o.source, staged[i], &e);
        }
        if (!ok) {
            DeleteTreeW(stageRoot);
            return Fail(AccessDenied(e) ? -32010 : -32012,
                        "Installation didn't complete, and nothing was changed. " + N(o.fromText ? o.rel : o.source) + ": " + Win32Message(e),
                        o.fromText ? g + L"\\" + o.rel : o.source, e);
        }
    }
    progress("Prepared files");
    // 2. Back up what will be replaced or removed.
    std::vector<ManifestFile> manifest;
    std::wstring backupDir;
    std::string backupId;
    bool anyBackup = false;
    for (const auto& o : ops)
        if (o.backup && (o.kind == OpKind::Replace || o.kind == OpKind::Remove || o.kind == OpKind::Merge) && FileExists(g + L"\\" + o.rel))
            anyBackup = true;
    if (anyBackup) {
        backupId = StampLocal() + "-" + action;
        backupDir = dataDir + L"\\backup\\" + W(backupId);
        for (int n = 2; DirExists(backupDir); ++n) {
            backupId = StampLocal() + "-" + action + "-" + std::to_string(n);
            backupDir = dataDir + L"\\backup\\" + W(backupId);
        }
    }
    for (const auto& o : ops) {
        const std::wstring target = g + L"\\" + o.rel;
        ManifestFile mf;
        mf.path = N(o.rel);
        mf.kind = o.kindLabel;
        mf.description = o.description;
        if (o.kind == OpKind::Add) {
            mf.op = "added";
            mf.sha256 = o.fromText ? hashutil::Sha256Hex(o.text.data(), o.text.size()) : hashutil::Sha256HexFile(o.source);
            manifest.push_back(mf);
            continue;
        }
        if (o.kind == OpKind::RemoveDir || o.kind == OpKind::Rename || !o.backup || !FileExists(target)) continue;
        mf.op = o.kind == OpKind::Remove ? "removed" : "replaced";
        mf.sha256 = hashutil::Sha256HexFile(target);
        mf.size = FileSize(target);
        const std::wstring dst = backupDir + L"\\" + o.rel;
        unsigned long e = 0;
        if (!MakeDirs(Parent(dst)) || !CopyVerified(target, dst, &e)) {
            if (!e) e = GetLastError();
            DeleteTreeW(stageRoot);
            if (!backupDir.empty()) DeleteTreeW(backupDir);
            return Fail(AccessDenied(e) ? -32010 : -32012, "Installation didn't complete, and nothing was changed. Could not back up " + mf.path + ": " + Win32Message(e),
                        target, e);
        }
        manifest.push_back(mf);
    }
    if (anyBackup) {
        jsonmini::Arr files;
        for (const auto& f : manifest) {
            jsonmini::Obj o;
            o.Str("path", f.path).Str("op", f.op).Str("sha256", f.sha256);
            if (f.size) o.UInt("size", f.size);
            if (!f.kind.empty()) o.Str("kind", f.kind);
            if (!f.description.empty()) o.Str("description", f.description);
            files.Raw(o.End());
        }
        const std::string mj = jsonmini::Obj().Int("version", 1).Str("created", NowIsoUtc()).Str("action", action).Str("melange", c.version)
                                   .Raw("files", files.End()).End();
        const unsigned long e = WriteAtomic(backupDir + L"\\manifest.json", mj);
        if (e) {
            DeleteTreeW(stageRoot);
            DeleteTreeW(backupDir);
            return Fail(AccessDenied(e) ? -32010 : -32012, "Could not write the backup record: " + Win32Message(e), backupDir, e);
        }
    }
    progress("Backed up");
    // 3. Commit by rename; anything that was in the way is moved aside into staging so it can be put back.
    std::vector<Done> done;
    unsigned long err = 0;
    std::wstring errPath;
    for (size_t i = 0; i < ops.size() && !err; ++i) {
        const Op& o = ops[i];
        const std::wstring target = g + L"\\" + o.rel;
        const std::wstring aside = stageRoot + L"\\old-" + std::to_wstring(i);
        switch (o.kind) {
            case OpKind::Add:
            case OpKind::Replace:
            case OpKind::Merge: {
                const bool had = FileExists(target);
                if (had) {
                    if ((err = Move(c, target, aside)) != 0) {
                        errPath = target;
                        break;
                    }
                    done.push_back(Done{Done::MovedAway, target, aside});
                }
                MakeDirs(Parent(target));
                if ((err = Move(c, staged[i], target)) != 0) {
                    errPath = target;
                    break;
                }
                done.push_back(Done{Done::Placed, target, {}});
                break;
            }
            case OpKind::Remove:
            case OpKind::RemoveDir: {
                if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) break;
                if (o.kind == OpKind::Remove && !o.expectSha.empty() && hashutil::Sha256HexFile(target) != o.expectSha) break;
                if ((err = Move(c, target, aside)) != 0) {
                    errPath = target;
                    break;
                }
                done.push_back(Done{Done::MovedAway, target, aside});
                break;
            }
            case OpKind::Rename: {
                const std::wstring to = g + L"\\" + o.source;
                if ((err = Move(c, target, to)) != 0) {
                    errPath = target;
                    break;
                }
                done.push_back(Done{Done::Placed, to, {}});
                done.push_back(Done{Done::MovedAway, target, to});
                break;
            }
        }
        if (!err) progress(N(o.rel));
    }
    if (err) {
        // Undo in reverse: placed files go back to staging (dropped), moved-away files return.
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            if (it->kind == Done::Placed) {
                if (!it->aside.empty()) continue;
                MoveFileExW(it->target.c_str(), (stageRoot + L"\\undo-" + W(RandomHex(4))).c_str(), MOVEFILE_REPLACE_EXISTING);
            } else {
                MoveFileExW(it->aside.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        DeleteTreeW(stageRoot);
        if (!backupDir.empty()) DeleteTreeW(backupDir);
        Outcome o = Fail(AccessDenied(err) ? -32010 : -32012,
                         AccessDenied(err) ? "Windows didn't let us write to the game folder."
                                           : "Installation didn't complete, and nothing was changed. " + N(errPath) + ": " + Win32Message(err),
                         errPath, err);
        return o;
    }
    DeleteTreeW(stageRoot);
    const std::wstring stagingParent = dataDir + L"\\.staging";
    RemoveDirectoryW(stagingParent.c_str());
    Outcome ok;
    ok.ok = true;
    ok.backupId = backupId;
    if (backupIdOut) *backupIdOut = backupId;
    if (recordOut) *recordOut = manifest;
    return ok;
}

std::string LoadBackupJson(const std::wstring& dir, Backup* out) {
    json::Value v;
    json::Error e;
    if (!json::ParseFile(dir + L"\\manifest.json", &v, &e) || !v.IsObject()) return "bad";
    auto s = [](const json::Value& o, const char* k) {
        const json::Value* x = o.Get(k);
        return x && x->IsString() ? x->string : std::string();
    };
    out->created = s(v, "created");
    out->action = s(v, "action");
    if (const json::Value* f = v.Get("files"); f && f->IsArray())
        for (const auto& it : f->items) {
            if (!it.IsObject()) continue;
            Backup::File bf{s(it, "path"), s(it, "op"), s(it, "sha256"), s(it, "kind"), s(it, "description")};
            if (bf.path.empty() || bf.path.find("..") != std::string::npos || bf.path.find(':') != std::string::npos ||
                bf.path[0] == '\\' || bf.path[0] == '/')
                continue;
            out->files.push_back(bf);
        }
    return {};
}
}  // namespace

unsigned long DefaultMove(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : GetLastError();
}

std::vector<std::wstring> ProtectFromEnv() {
    std::vector<std::wstring> out;
    wchar_t buf[8192];
    const DWORD n = GetEnvironmentVariableW(L"MELANGE_PROTECT", buf, 8192);
    if (!n || n >= 8192) return out;
    std::wstring s(buf, n);
    size_t i = 0;
    while (i <= s.size()) {
        const size_t semi = s.find(L';', i);
        std::wstring part = s.substr(i, semi == std::wstring::npos ? std::wstring::npos : semi - i);
        while (!part.empty() && part.front() == L' ') part.erase(part.begin());
        while (!part.empty() && part.back() == L' ') part.pop_back();
        if (!part.empty()) out.push_back(FullPath(part));
        if (semi == std::wstring::npos) break;
        i = semi + 1;
    }
    return out;
}

std::vector<Backup> ListBackups(const std::wstring& gameDir) {
    std::vector<Backup> out;
    if (gameDir.empty()) return out;
    const std::wstring root = gameDir + L"\\Melange\\backup";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        Backup b;
        b.id = N(fd.cFileName);
        if (!SafeId(b.id) || !LoadBackupJson(root + L"\\" + fd.cFileName, &b).empty()) continue;
        out.push_back(std::move(b));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const Backup& a, const Backup& b) { return a.created != b.created ? a.created > b.created : a.id > b.id; });
    return out;
}

Status Inspect(const Context& c) {
    Status s;
    s.payload = ReadPayload(c);
    if (c.gameDir.empty()) return s;
    const std::wstring g = c.gameDir;
    s.haveGame = true;
    s.game = CheckExe(g, c.profiles ? *c.profiles : DefaultProfiles());
    if (c.storeOf) s.game.store = c.storeOf(g);
    s.running = Running(c);
    s.game.running = s.running;
    s.game.writable = DirExists(g) && CanWrite(g);
    s.melangeLoaded = s.running && Loaded(c);
    if (!DirExists(g)) return s;

    const std::string ours = s.payload.ualSha;
    if (IdentifyDll(g + L"\\dinput8.dll", ours, &s.loader)) {
        s.haveLoader = true;
        s.loaderState = s.loader.kind == DllKind::Ual ? "ual" : "other";
    }
    for (const wchar_t* alt : kAltLoaders) {
        DllInfo d;
        if (IdentifyDll(g + L"\\" + alt, ours, &d) && d.kind == DllKind::Ual) s.otherLoaders.push_back(d);
    }

    std::vector<std::wstring> asis, offs;
    const std::wstring asi = Found(g, L"melange.asi", &asis);
    Found(g, L"melange.asi.off", &offs);
    for (size_t i = 1; i < asis.size(); ++i) s.duplicates.push_back(N(asis[i]));
    if (!asi.empty()) {
        s.melangePath = N(asi);
        s.melangeVersion = FileProductVersion(g + L"\\" + asi);
        if (s.melangeVersion.empty()) {
            s.melangeState = "damaged";
        } else {
            const int cmp = CompareVersions(s.melangeVersion, c.version);
            if (cmp < 0) s.melangeState = "older";
            else if (cmp > 0) s.melangeState = "newer";
            else s.melangeState = "installed";
        }
    } else if (!offs.empty()) {
        s.melangeState = "disabled";
        s.melangePath = N(offs.front());
        s.melangeVersion = FileProductVersion(g + L"\\" + offs.front());
    }
    std::vector<std::wstring> legacy;
    Found(g, L"WUMFix.asi", &legacy);
    for (const auto& l : legacy) s.legacy.push_back(N(l));
    if (FileExists(g + L"\\oasis.exe")) s.legacy.push_back("oasis.exe");

    s.iniPresent = FileExists(g + L"\\Melange.ini");
    if (s.iniPresent && !c.payloadDir.empty()) {
        std::string templ;
        if (ReadAll(c.payloadDir + L"\\Melange.ini", &templ, 4u << 20)) {
            oasis::ini::Encoding e1{}, e2{};
            s.iniMissing = IniMissingKeys(oasis::ini::Decode(templ, &e1), ReadIniText(g + L"\\Melange.ini", &e2));
        }
    }
    s.backups = ListBackups(g);
    s.install = ReadInstallRecord(g);
    if (s.melangeState != "missing") LastLoad(c.logsDir, &s.lastLoadAt, &s.lastLoadVersion);
    return s;
}

std::string StatusJson(const Status& s) {
    jsonmini::Obj o;
    o.Raw("game", s.haveGame ? GameCheckJson(s.game) : "null").Bool("running", s.running).Bool("melangeLoaded", s.melangeLoaded);
    jsonmini::Obj loader;
    loader.Str("state", s.loaderState);
    if (s.haveLoader) loader.Raw("dll", DllInfoJson(s.loader));
    o.Raw("loader", loader.End());
    jsonmini::Arr others;
    for (const auto& d : s.otherLoaders) others.Raw(DllInfoJson(d));
    o.Raw("otherLoaders", others.End());
    jsonmini::Obj m;
    m.Str("state", s.melangeState);
    if (!s.melangeVersion.empty()) m.Str("version", s.melangeVersion);
    if (!s.melangePath.empty()) m.Str("path", s.melangePath);
    jsonmini::Arr dups;
    for (const auto& d : s.duplicates) dups.Str(d);
    m.Raw("duplicates", dups.End());
    if (!s.lastLoadAt.empty()) m.Raw("lastLoad", jsonmini::Obj().Str("at", s.lastLoadAt).Str("version", s.lastLoadVersion).End());
    o.Raw("melange", m.End());
    o.Raw("ini", jsonmini::Obj().Bool("present", s.iniPresent).Int("missingKeys", s.iniMissing).End());
    jsonmini::Arr legacy;
    for (const auto& l : s.legacy) legacy.Str(l);
    o.Raw("legacy", legacy.End());
    jsonmini::Arr miss;
    for (const auto& x : s.payload.missing) miss.Str(x);
    o.Raw("payload", jsonmini::Obj().Bool("ok", s.payload.ok).Str("version", s.payload.version).Raw("missing", miss.End())
                         .Bool("fromGameFolder", s.payload.fromGameFolder).End());
    jsonmini::Arr backups;
    for (const auto& b : s.backups) {
        jsonmini::Arr files;
        for (const auto& f : b.files) {
            jsonmini::Obj fo;
            fo.Str("path", f.path).Str("op", f.op);
            if (!f.description.empty()) fo.Str("description", f.description);
            if (!f.kind.empty()) fo.Str("kind", f.kind);
            files.Raw(fo.End());
        }
        backups.Raw(jsonmini::Obj().Str("id", b.id).Str("created", b.created).Str("action", b.action).Raw("files", files.End()).End());
    }
    o.Raw("backups", backups.End());
    if (s.install.present)
        o.Raw("install", jsonmini::Obj().Str("melange", s.install.melange).Str("installedAt", s.install.installedAt)
                             .Str("loader", s.install.loader).End());
    if (s.busyActive)
        o.Raw("busy", jsonmini::Obj().Str("action", s.busyAction).Int("step", s.busyStep).Int("of", s.busyOf).Str("label", s.busyLabel).End());
    return o.End();
}

std::string WriteGate(const Context& c) { return Gate(c, false, nullptr); }
std::string GameGate(const Context& c) { return Gate(c, true, nullptr); }

Plan MakePlan(const Context& c, const PlanRequest& req) { return Build(c, req, nullptr).plan; }

std::string PlanJson(const Plan& p) {
    jsonmini::Arr steps;
    for (const auto& s : p.steps) steps.Raw(jsonmini::Obj().Str("op", s.op).Str("path", s.path).Str("detail", s.detail).End());
    jsonmini::Obj o;
    o.Str("planId", p.planId).Raw("steps", steps.End());
    if (!p.needsChoice.empty()) o.Str("needsChoice", p.needsChoice);
    if (!p.refused.empty()) o.Str("refused", p.refused);
    if (!p.missing.empty()) {
        jsonmini::Arr m;
        for (const auto& x : p.missing) m.Str(x);
        o.Raw("missing", m.End());
    }
    return o.End();
}

Outcome Apply(const Context& c, const PlanRequest& req, const std::string& planId) {
    if (req.action != "install" && req.action != "repair" && req.action != "uninstall") return Fail(-32602, "action must be install, repair or uninstall");
    Status st;
    Built b = Build(c, req, &st);
    if (!b.plan.refused.empty()) {
        Outcome o = Fail(b.plan.code ? b.plan.code : -32000, b.plan.refused);
        o.missing = b.plan.missing;
        return o;
    }
    if (!planId.empty() && planId != b.plan.planId) return Fail(-32013, "The game folder changed since the plan was shown. Check the new plan and confirm again.");
    if (!b.plan.needsChoice.empty()) return Fail(-32000, "Another program's dinput8.dll is in your game folder. Choose whether to replace it.");
    std::string backupId;
    Outcome o = RunOps(c, req.action, b.ops, &backupId);
    if (!o.ok) return o;
    const std::wstring rec = c.gameDir + L"\\Melange\\install.json";
    if (req.action != "uninstall") {
        std::string loaderMode = b.loaderMode, loaderSha = b.loaderSha;
        if (st.install.present && loaderMode == "reused" && (st.install.loader == "added" || st.install.loader == "replaced") &&
            st.install.loaderSha256 == st.loader.sha256)
            loaderMode = st.install.loader;   // a repair keeps knowing who put the loader there
        jsonmini::Obj files;
        for (const wchar_t* f : {L"melange.asi", L"dinput8.dll", L"Melange.exe"})
            if (FileExists(c.gameDir + L"\\" + f)) files.Str(N(f), hashutil::Sha256HexFile(c.gameDir + L"\\" + f));
        const std::string j = jsonmini::Obj().Int("version", 1).Str("melange", c.version).Str("installedAt", NowIsoUtc())
                                  .Str("loader", loaderMode).Str("loaderSha256", loaderSha).Raw("files", files.End()).End();
        MakeDirs(c.gameDir + L"\\Melange");
        WriteAtomic(rec, j);
    }
    return o;
}

Outcome Restore(const Context& c, const std::string& backupId) {
    if (!SafeId(backupId)) return Fail(-32602, "unknown backup");
    const std::wstring dir = c.gameDir + L"\\Melange\\backup\\" + W(backupId);
    Backup b;
    if (!DirExists(dir) || !LoadBackupJson(dir, &b).empty()) return Fail(-32602, "unknown backup");
    bool exists = false;
    const std::string gate = Gate(c, false, &exists);
    if (!gate.empty()) return Fail(-32000, gate);
    std::vector<Op> ops;
    for (const auto& f : b.files) {
        const std::wstring rel = W(f.path);
        if (f.op == "replaced" || f.op == "removed") {
            const std::wstring src = dir + L"\\" + rel;
            if (!FileExists(src)) return Fail(-32000, "The backup is incomplete: " + f.path + " is missing from it.");
            Op o{FileExists(c.gameDir + L"\\" + rel) ? OpKind::Replace : OpKind::Add, rel, src};
            ops.push_back(o);
        } else if (f.op == "added") {
            Op o{OpKind::Remove, rel};
            o.expectSha = f.sha256;
            ops.push_back(o);
        }
    }
    // The loader last, as on install.
    std::stable_partition(ops.begin(), ops.end(), [](const Op& o) { return _wcsicmp(o.rel.c_str(), L"dinput8.dll") != 0; });
    return RunOps(c, "restore", ops, nullptr);
}

Outcome DeleteBackup(const Context& c, const std::string& backupId) {
    if (!SafeId(backupId)) return Fail(-32602, "unknown backup");
    if (IsProtected(c)) return Fail(-32000, kProtectedCopy);
    const std::wstring dir = c.gameDir + L"\\Melange\\backup\\" + W(backupId);
    if (!DirExists(dir)) return Fail(-32602, "unknown backup");
    if (!DeleteTreeW(dir)) {
        const unsigned long e = GetLastError();
        return Fail(AccessDenied(e) ? -32010 : -32000, "Could not delete the backup: " + Win32Message(e), dir, e);
    }
    Outcome o;
    o.ok = true;
    return o;
}

Outcome SetMelangeEnabled(const Context& c, bool on) {
    const std::string gate = Gate(c, false, nullptr);
    if (!gate.empty()) return Fail(-32000, gate);
    const std::wstring from = Found(c.gameDir, on ? L"melange.asi.off" : L"melange.asi");
    if (from.empty()) {
        if (!Found(c.gameDir, on ? L"melange.asi" : L"melange.asi.off").empty()) {
            Outcome o;
            o.ok = true;
            return o;
        }
        return Fail(-32000, "Melange isn't installed in this folder.");
    }
    const std::wstring to = on ? from.substr(0, from.size() - 4) : from + L".off";
    if (FileExists(c.gameDir + L"\\" + to)) return Fail(-32000, "Both melange.asi and melange.asi.off exist; run Repair first.");
    const unsigned long e = Move(c, c.gameDir + L"\\" + from, c.gameDir + L"\\" + to);
    if (e) return Fail(AccessDenied(e) ? -32010 : -32012, "Could not rename " + N(from) + ": " + Win32Message(e), c.gameDir + L"\\" + from, e);
    Outcome o;
    o.ok = true;
    return o;
}
}  // namespace melange::launcher::setup
