#include "erg/install.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "erg/xomutil.h"
#include "mods/spice.h"
#include "xom/xom.h"

namespace melange::erg::install {
namespace {
bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

bool ParseFile(const std::wstring& path, size_t max, xom::Document* doc, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, max, &bytes, err)) return false;
    xom::ParseOptions opt;
    opt.strict = true;
    return xom::parse(bytes.data(), bytes.size(), *doc, err, opt);
}

std::wstring Full(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    std::wstring s = n && n < std::size(buf) ? std::wstring(buf, n) : p;
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

template <class Fn>
void ForEach(const std::wstring& pattern, Fn fn) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name != L"." && name != L"..") fn(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool SafeRel(const std::string& rel) {
    if (rel.empty() || rel.size() > 120 || rel.find("..") != std::string::npos || rel.find(':') != std::string::npos ||
        rel[0] == '\\' || rel[0] == '/')
        return false;
    for (unsigned char c : rel)
        if (c < 0x20 || c > 0x7e || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    return true;
}
}  // namespace

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ValidFileStem(std::string_view s) {
    if (s.empty() || s.size() > 63) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool Exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadFile(const std::wstring& path, size_t max, std::vector<uint8_t>* out, std::string* err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Fail(err, "cannot open " + Narrow(path));
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != 0;
    if (ok && static_cast<uint64_t>(size.QuadPart) > max) {
        CloseHandle(h);
        return Fail(err, Narrow(path) + " is larger than " + std::to_string(max >> 20) + " MB");
    }
    out->assign(ok ? static_cast<size_t>(size.QuadPart) : 0, 0);
    size_t got = 0;
    while (ok && got < out->size()) {
        DWORD n = 0;
        ok = ::ReadFile(h, out->data() + got, static_cast<DWORD>(std::min<size_t>(out->size() - got, 1u << 20)), &n, nullptr) && n;
        got += n;
    }
    CloseHandle(h);
    return ok || Fail(err, "cannot read " + Narrow(path));
}

bool WriteAtomic(const std::wstring& path, const void* data, size_t n, std::string* err) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Fail(err, "cannot write " + Narrow(path));
    size_t put = 0;
    bool ok = true;
    while (ok && put < n) {
        DWORD w = 0;
        ok = WriteFile(h, static_cast<const uint8_t*>(data) + put, static_cast<DWORD>(std::min<size_t>(n - put, 1u << 20)), &w, nullptr) && w;
        put += w;
    }
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return Fail(err, "cannot write " + Narrow(path));
    }
    return true;
}

bool MakeDirs(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2 && !MakeDirs(dir.substr(0, slash))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool Inside(const std::wstring& path, const std::wstring& dir) {
    const std::wstring p = Full(path), d = Full(dir);
    return p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == L'\\';
}

bool NoReparse(const std::wstring& top, const std::wstring& dir) {
    if (dir != top && !Inside(dir, top)) return false;
    std::wstring cur = dir;
    for (;;) {
        const DWORD a = GetFileAttributesW(cur.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (Full(cur) == Full(top)) return true;
        const size_t slash = cur.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        cur = cur.substr(0, slash);
    }
}

std::wstring DataDir(const std::wstring& gameDir) { return gameDir + L"\\Data"; }

bool ReadRegistry(const std::wstring& gameDir, std::vector<RegistryEntry>* out, std::string* err) {
    xom::Document s;
    if (!ParseFile(DataDir(gameDir) + L"\\Tweak\\SCRIPTS.XOM", 16u << 20, &s, err)) return false;
    out->clear();
    for (const auto& o : s.objects) {
        if (o.type != "XContainerResourceDetails") continue;
        const xom::Value* v = o.field("Value");
        const xom::Object* l = v && v->type == xom::Type::Ref && !v->array ? s.object(v->asRef()) : nullptr;
        if (!l || l->type != "WXFE_LevelDetails") continue;
        RegistryEntry e;
        e.key = xomutil::Str(o, "Name");
        e.file = xomutil::Str(*l, "Level_FileName");
        e.frontendName = xomutil::Str(*l, "Frontend_Name");
        e.scripts = xomutil::Str(*l, "Level_ScriptName");
        e.lock = xomutil::Str(*l, "Lock");
        e.levelType = static_cast<int>(xomutil::Int(*l, "Level_Type", -1));
        e.themeType = static_cast<int>(xomutil::Int(*l, "Theme_Type", 5));
        e.previewType = static_cast<int>(xomutil::Int(*l, "Preview_Type", 0));
        out->push_back(std::move(e));
    }
    return true;
}

std::map<std::string, std::string> ReadFrontendStrings(const std::wstring& gameDir) {
    std::map<std::string, std::string> out;
    xom::Document d;
    if (!ParseFile(DataDir(gameDir) + L"\\Language\\PC\\EngFE.xom", 16u << 20, &d, nullptr)) return out;
    for (const auto& o : d.objects)
        if (o.type == "XStringResourceDetails") out[xomutil::Str(o, "Name")] = xomutil::Str(o, "Value");
    return out;
}

std::vector<std::string> MaterialFiles(const std::wstring& gameDir) {
    std::vector<std::string> out;
    const std::wstring themes = DataDir(gameDir) + L"\\Themes";
    ForEach(themes + L"\\Theme*", [&](const std::wstring& dir, bool isDir) {
        if (isDir && Exists(themes + L"\\" + dir + L"\\" + dir + L".txt")) out.push_back(Narrow(dir + L"\\" + dir + L".txt"));
    });
    ForEach(DataDir(gameDir) + L"\\Maps\\*.txt", [&](const std::wstring& f, bool isDir) {
        if (!isDir) out.push_back("Maps\\" + Narrow(f));
    });
    std::sort(out.begin(), out.end());
    return out;
}

bool MaterialFileExists(const std::wstring& gameDir, const std::string& rel) {
    if (!SafeRel(rel)) return false;
    std::wstring w = Widen(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    const std::wstring data = DataDir(gameDir);
    return Exists(data + L"\\" + w) || Exists(data + L"\\Themes\\" + w);
}

bool ReadMaterialFile(const std::wstring& gameDir, const std::string& rel, std::vector<uint8_t>* out, std::string* err) {
    if (!SafeRel(rel)) {
        if (err) *err = "'" + rel + "' is not a relative path inside the install";
        return false;
    }
    std::wstring w = Widen(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    const std::wstring data = DataDir(gameDir);
    const std::wstring path = Exists(data + L"\\" + w) ? data + L"\\" + w : data + L"\\Themes\\" + w;
    return ReadFile(path, 1u << 20, out, err);
}

std::vector<std::string> MaterialNames(const std::vector<uint8_t>& txt) {
    std::vector<std::string> lines(1);
    for (uint8_t c : txt) {
        if (c == '\n') lines.emplace_back();
        else if (c != '\r') lines.back() += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
    }
    // Records start at the next non-blank line: Diner Might's file has a second blank line after record 29.
    std::vector<std::string> out;
    size_t at = 0;
    while (out.size() < 64) {
        while (at < lines.size() && lines[at].empty()) ++at;
        if (at + 4 >= lines.size()) break;
        out.push_back(lines[at + 4].substr(0, 63));
        at += 6;
    }
    return out;
}

bool PackFromManifest(const spice::Manifest& m, const std::wstring& dir, Pack* out) {
    if (m.levels.empty()) return false;
    std::vector<levels::manifest::Error> errs;
    std::vector<levels::manifest::LevelDecl> decls = levels::manifest::Parse(m, &errs);
    if (decls.empty()) return false;
    out->modId = m.id;
    out->dir = dir;
    out->assetsRoot = m.assetsRoot.empty() ? "assets" : m.assetsRoot;
    out->levels = std::move(decls);
    return true;
}

std::wstring LevelRoot(const Pack& p) {
    std::wstring root = Widen(p.assetsRoot);
    std::replace(root.begin(), root.end(), L'/', L'\\');
    while (!root.empty() && root.back() == L'\\') root.pop_back();
    return p.dir + L"\\" + root + L"\\levels";
}

std::vector<Pack> ScanPacks(const std::wstring& gameDir) {
    std::vector<Pack> out;
    const std::wstring mods = gameDir + L"\\Mods";
    ForEach(mods + L"\\*", [&](const std::wstring& name, bool isDir) {
        if (!isDir) return;
        spice::Manifest m;
        std::vector<spice::Error> errs;
        Pack p;
        if (spice::Parse(mods + L"\\" + name, &m, &errs) && PackFromManifest(m, mods + L"\\" + name, &p)) out.push_back(std::move(p));
    });
    std::sort(out.begin(), out.end(), [](const Pack& a, const Pack& b) { return a.modId < b.modId; });
    return out;
}

std::vector<Pack> AssignPacks(std::vector<Pack> inLoadOrder) {
    std::vector<std::vector<levels::manifest::LevelDecl>> per;
    for (const auto& p : inLoadOrder) per.push_back(p.levels);
    const std::vector<levels::manifest::LevelDecl> kept = levels::manifest::Assign(per, nullptr);
    std::vector<Pack> out;
    for (auto& p : inLoadOrder)
        if (std::any_of(kept.begin(), kept.end(), [&](const levels::manifest::LevelDecl& d) { return d.mod == p.modId; }))
            out.push_back(std::move(p));
    return out;
}
}  // namespace melange::erg::install
