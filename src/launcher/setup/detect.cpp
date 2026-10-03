#include "launcher/setup/detect.h"

#include <windows.h>

#include <set>

#include "launcher/setup/vdf.h"
#include "launcher/util.h"
#include "tools/json_mini.h"

namespace melange::launcher::setup {
namespace {
constexpr wchar_t kAppId[] = L"70600";
const wchar_t* const kUninstallKeys[] = {L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                                         L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"};
const wchar_t* const kGogKeys[] = {L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", L"SOFTWARE\\GOG.com\\Games"};

class WinReg final : public RegReader {
  public:
    bool String(Hive hive, const std::wstring& key, const std::wstring& value, std::wstring* out) override {
        HKEY k{};
        if (RegOpenKeyExW(Root(hive), key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
        DWORD type = 0, bytes = 0;
        bool ok = RegQueryValueExW(k, value.c_str(), nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
                  (type == REG_SZ || type == REG_EXPAND_SZ) && bytes < 65536;
        if (ok) {
            std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
            ok = RegQueryValueExW(k, value.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &bytes) == ERROR_SUCCESS;
            if (ok) {
                buf.resize(wcsnlen(buf.c_str(), buf.size()));
                *out = buf;
            }
        }
        RegCloseKey(k);
        return ok;
    }
    std::vector<std::wstring> Subkeys(Hive hive, const std::wstring& key) override {
        std::vector<std::wstring> out;
        HKEY k{};
        if (RegOpenKeyExW(Root(hive), key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return out;
        wchar_t name[256];
        for (DWORD i = 0; out.size() < 4096; ++i) {
            DWORD n = 256;
            if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            out.emplace_back(name, n);
        }
        RegCloseKey(k);
        return out;
    }

  private:
    static HKEY Root(Hive h) { return h == Hive::CurrentUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
};

std::wstring SteamRoot(RegReader& reg) {
    std::wstring p;
    if (reg.String(Hive::CurrentUser, L"Software\\Valve\\Steam", L"SteamPath", &p) && !p.empty()) return FullPath(p);
    if (reg.String(Hive::LocalMachine, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", &p) && !p.empty()) return FullPath(p);
    if (reg.String(Hive::LocalMachine, L"SOFTWARE\\Valve\\Steam", L"InstallPath", &p) && !p.empty()) return FullPath(p);
    return {};
}

std::vector<std::wstring> GogFolders(RegReader& reg) {
    std::vector<std::wstring> out;
    for (const wchar_t* base : kGogKeys) {
        for (const auto& sub : reg.Subkeys(Hive::LocalMachine, base)) {
            const std::wstring key = std::wstring(base) + L"\\" + sub;
            std::wstring exe, name, path;
            reg.String(Hive::LocalMachine, key, L"exe", &exe);
            reg.String(Hive::LocalMachine, key, L"gameName", &name);
            const std::wstring lexe = LowerW(exe);
            const bool isExe = lexe.size() >= 15 && lexe.compare(lexe.size() - 15, 15, L"wormsmayhem.exe") == 0;
            if ((isExe || IContains(Narrow(name), "Worms Ultimate Mayhem")) && reg.String(Hive::LocalMachine, key, L"path", &path) &&
                !path.empty())
                out.push_back(FullPath(path));
        }
    }
    return out;
}

struct Builder {
    std::vector<Candidate> list;
    std::set<std::wstring> seen;
    void Add(const std::wstring& path, const char* source, const std::wstring& library = {}) {
        if (path.empty()) return;
        const std::wstring full = FullPath(path);
        if (!seen.insert(LowerW(full)).second) return;
        Candidate c;
        c.path = full;
        c.source = source;
        c.library = library;
        list.push_back(std::move(c));
    }
};
}  // namespace

RegReader& SystemRegistry() {
    static WinReg r;
    return r;
}

std::vector<std::wstring> SteamLibraries(RegReader& reg) {
    std::vector<std::wstring> libs;
    const std::wstring root = SteamRoot(reg);
    if (root.empty()) return libs;
    std::set<std::wstring> seen;
    std::string text;
    if (ReadAll(root + L"\\steamapps\\libraryfolders.vdf", &text, vdf::kMaxBytes)) {
        vdf::Node n;
        vdf::Parse(text, &n);
        for (const auto& l : vdf::Libraries(n, "70600")) {
            const std::wstring full = FullPath(Widen(l));
            if (seen.insert(LowerW(full)).second) libs.push_back(full);
        }
    }
    if (seen.insert(LowerW(root)).second) libs.push_back(root);
    return libs;
}

std::string StoreOf(const std::wstring& dir, RegReader& reg) {
    for (const auto& lib : SteamLibraries(reg))
        if (PathInside(dir, lib + L"\\steamapps\\common") && PathKey(dir) != PathKey(lib + L"\\steamapps\\common")) return "steam";
    for (const auto& g : GogFolders(reg))
        if (PathKey(g) == PathKey(dir)) return "gog";
    return "unknown";
}

std::wstring ChildWithGame(const std::wstring& dir) {
    const std::wstring base = FullPath(dir);
    if (FileExists(base + L"\\WormsMayhem.exe")) return {};
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    std::wstring found;
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring sub = base + L"\\" + fd.cFileName;
        if (FileExists(sub + L"\\WormsMayhem.exe")) {
            found = sub;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found.empty() && FileExists(base + L"\\steamapps\\common\\WormsXHD\\WormsMayhem.exe")) found = base + L"\\steamapps\\common\\WormsXHD";
    if (found.empty() && FileExists(base + L"\\common\\WormsXHD\\WormsMayhem.exe")) found = base + L"\\common\\WormsXHD";
    return found;
}

std::vector<Candidate> Detect(const DetectInput& in) {
    RegReader& reg = in.reg ? *in.reg : SystemRegistry();
    const std::vector<Profile>& profiles = in.profiles ? *in.profiles : DefaultProfiles();
    Builder b;
    if (!in.saved.empty()) b.Add(in.saved, "saved");
    if (!in.selfDir.empty() && FileExists(in.selfDir + L"\\WormsMayhem.exe")) b.Add(in.selfDir, "self");
    for (const auto& lib : SteamLibraries(reg)) {
        std::string acf;
        std::wstring installDir;
        if (ReadAll(lib + L"\\steamapps\\appmanifest_" + kAppId + L".acf", &acf, vdf::kMaxBytes)) {
            vdf::Node n;
            vdf::Parse(acf, &n);
            installDir = Widen(vdf::InstallDir(n));
        }
        if (installDir.empty() || installDir.find_first_of(L"\\/:") != std::wstring::npos) installDir.clear();
        if (!installDir.empty()) b.Add(lib + L"\\steamapps\\common\\" + installDir, "steam", lib);
        else if (DirExists(lib + L"\\steamapps\\common\\WormsXHD")) b.Add(lib + L"\\steamapps\\common\\WormsXHD", "steam", lib);
    }
    for (const wchar_t* base : kUninstallKeys) {
        std::wstring loc;
        if (reg.String(Hive::LocalMachine, std::wstring(base) + L"\\Steam App " + kAppId, L"InstallLocation", &loc)) b.Add(loc, "steam");
    }
    for (const auto& g : GogFolders(reg)) b.Add(g, "gog");
    for (const wchar_t* base : kUninstallKeys)
        for (const auto& sub : reg.Subkeys(Hive::LocalMachine, base)) {
            if (sub.rfind(L"Steam App ", 0) == 0) continue;
            std::wstring name, loc;
            const std::wstring key = std::wstring(base) + L"\\" + sub;
            if (reg.String(Hive::LocalMachine, key, L"DisplayName", &name) && IContains(Narrow(name), "Worms Ultimate Mayhem") &&
                reg.String(Hive::LocalMachine, key, L"InstallLocation", &loc))
                b.Add(loc, "gog");
        }
    for (auto& c : b.list) {
        c.check = CheckExe(c.path, profiles);
        c.check.store = c.source == "steam" ? "steam" : c.source == "gog" ? "gog" : StoreOf(c.path, reg);
    }
    return b.list;
}

std::string CandidateJson(const Candidate& c) {
    jsonmini::Obj o;
    o.Str("path", Narrow(c.path)).Str("source", c.source);
    if (!c.library.empty()) o.Str("library", Narrow(c.library));
    o.Raw("check", GameCheckJson(c.check));
    return o.End();
}
}  // namespace melange::launcher::setup
