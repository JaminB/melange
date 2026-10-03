// Offline self-test for Melange.exe's setup logic: VDF parsing, game detection, exe validation, loader identity,
// the install/repair/uninstall/restore engine, Melange.ini merging, plugin settings and the recommended set. Works
// on fake game folders under %TEMP%; never touches a real game folder.
// Exit code 0 = all passed.
#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "launcher/plugin_settings.h"
#include "launcher/recommended.h"
#include "launcher/settings.h"
#include "launcher/setup/detect.h"
#include "launcher/setup/dll_id.h"
#include "launcher/setup/engine.h"
#include "launcher/setup/exe_check.h"
#include "launcher/setup/ini_merge.h"
#include "launcher/setup/running.h"
#include "launcher/setup/vdf.h"
#include "launcher/util.h"
#include "store/index.h"
#include "store/store.h"
#include "tools/hash.h"
#include "tools/json_read.h"

#include <miniz.h>

namespace L = melange::launcher;
namespace S = melange::launcher::setup;
namespace vdf = melange::launcher::vdf;

namespace {
int g_pass = 0, g_fail = 0;
void Expect(bool ok, const char* what, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
    }
}

std::wstring g_tmp, g_bin, g_src;

void Put(const std::wstring& path, const std::string& text) {
    L::MakeDirs(L::Parent(path));
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        printf("cannot write %ls\n", path.c_str());
        return;
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}
std::string Get(const std::wstring& path) {
    std::string s;
    L::ReadAll(path, &s);
    return s;
}
void Copy(const std::wstring& from, const std::wstring& to) {
    L::MakeDirs(L::Parent(to));
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) printf("copy failed %ls -> %ls (%lu)\n", from.c_str(), to.c_str(), GetLastError());
}
void Wipe(const std::wstring& dir) {
    std::wstring from = dir + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}
std::wstring Fresh(const wchar_t* name) {
    const std::wstring d = g_tmp + L"\\" + name;
    Wipe(d);
    L::MakeDirs(d);
    return d;
}
// rel path (lower-case) -> sha256 of every file under `dir`, `skip` folders left out.
void Snapshot(const std::wstring& dir, const std::wstring& rel, std::map<std::wstring, std::string>* out, const std::wstring& skip) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\" + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring r = rel + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (_wcsicmp(r.c_str(), skip.c_str()) != 0) Snapshot(dir, r + L"\\", out, skip);
        } else {
            (*out)[L::LowerW(r)] = melange::hashutil::Sha256HexFile(dir + L"\\" + r);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
std::map<std::wstring, std::string> Snap(const std::wstring& dir, const std::wstring& skip = L"") {
    std::map<std::wstring, std::string> m;
    Snapshot(dir, L"", &m, skip);
    return m;
}

// ---------------------------------------------------------------- vdf
void TestVdf() {
    const std::string modern =
        "\xEF\xBB\xBF\"libraryfolders\"\r\n{\r\n"
        "\t\"0\"\r\n\t{\r\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\r\n\t\t\"label\"\t\t\"\"\r\n"
        "\t\t\"apps\"\r\n\t\t{\r\n\t\t\t\"228980\"\t\t\"1\"\r\n\t\t}\r\n\t}\r\n"
        "\t// a comment\r\n"
        "\t\"1\"\r\n\t{\r\n\t\t\"path\"\t\t\"D:\\\\Steam Library \\\"x\\\"\"\r\n\t\t\"apps\"\r\n\t\t{\r\n\t\t\t\"70600\"\t\t\"123\"\r\n\t\t}\r\n\t}\r\n"
        "}\r\n";
    vdf::Node n;
    Expect(vdf::Parse(modern, &n), "vdf: modern parses");
    auto libs = vdf::Libraries(n, "70600");
    Expect(libs.size() == 2, "vdf: two libraries");
    if (libs.size() == 2) {
        Expect(libs[0] == "D:\\Steam Library \"x\"", "vdf: the library listing the app comes first, escapes decoded", libs[0]);
        Expect(libs[1] == "C:\\Program Files (x86)\\Steam", "vdf: second library", libs[1]);
    }
    const std::string old = "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1700000000\"\n\t\"ContentStatsID\"\t\t\"-1\"\n\t\"1\"\t\t\"E:\\\\Games\\\\Steam\"\n}\n";
    Expect(vdf::Parse(old, &n), "vdf: old shape parses");
    libs = vdf::Libraries(n, "70600");
    Expect(libs.size() == 1 && libs[0] == "E:\\Games\\Steam", "vdf: old shape library");
    const std::string unicode = "\"libraryfolders\" { \"0\" { \"path\" \"F:\\\\Spiele \xC3\xBC\xE6\xB8\xB8\xE6\x88\x8F\" } }";
    Expect(vdf::Parse(unicode, &n) && vdf::Libraries(n, "70600").size() == 1 &&
               L::Widen(vdf::Libraries(n, "70600")[0]) == L"F:\\Spiele \u00fc\u6e38\u620f",
           "vdf: UTF-8 path, unquoted-free single line");
    const std::string truncated = "\"libraryfolders\" { \"0\" { \"path\" \"G:\\\\One\" } \"1\" { \"path\" \"H:\\\\Tw";
    Expect(!vdf::Parse(truncated, &n), "vdf: truncated file reports ok=false");
    libs = vdf::Libraries(n, "70600");
    Expect(libs.size() == 1 && libs[0] == "G:\\One", "vdf: truncated file keeps complete entries");
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "\"k\" { ";
    Expect(!vdf::Parse(deep, &n), "vdf: depth limit");
    std::string big(vdf::kMaxBytes + 10, ' ');
    Expect(!vdf::Parse(big, &n), "vdf: size limit");
    const std::string acf = "\"AppState\"\n{\n\t\"appid\"\t\t\"70600\"\n\t\"name\"\t\t\"Worms Ultimate Mayhem\"\n\t\"installdir\"\t\t\"WormsXHD\"\n}\n";
    Expect(vdf::Parse(acf, &n) && vdf::InstallDir(n) == "WormsXHD", "vdf: appmanifest installdir");
    Expect(vdf::Parse("\"a\" { unquoted value }", &n) && n.Get("a") && n.Get("a")->Str("unquoted") == "value", "vdf: unquoted tokens");
}

// ---------------------------------------------------------------- detection
class FakeReg final : public S::RegReader {
  public:
    std::map<std::wstring, std::wstring> values;   // "H|key|value" -> data, H = U or M
    bool String(S::Hive hive, const std::wstring& key, const std::wstring& value, std::wstring* out) override {
        auto it = values.find(K(hive, key) + L"|" + L::LowerW(value));
        if (it == values.end()) return false;
        *out = it->second;
        return true;
    }
    std::vector<std::wstring> Subkeys(S::Hive hive, const std::wstring& key) override {
        std::vector<std::wstring> out;
        const std::wstring prefix = K(hive, key) + L"\\";
        for (const auto& [k, v] : values) {
            (void)v;
            if (k.rfind(prefix, 0) != 0) continue;
            const std::wstring rest = k.substr(prefix.size());
            const std::wstring sub = rest.substr(0, rest.find_first_of(L"\\|"));
            bool dup = false;
            for (const auto& o : out) dup |= o == sub;
            if (!dup) out.push_back(sub);
        }
        return out;
    }
    void Set(S::Hive hive, const std::wstring& key, const std::wstring& value, const std::wstring& data) {
        values[K(hive, key) + L"|" + L::LowerW(value)] = data;
    }

  private:
    static std::wstring K(S::Hive h, const std::wstring& key) { return (h == S::Hive::CurrentUser ? L"U|" : L"M|") + L::LowerW(key); }
};

std::vector<S::Profile> FakeProfiles(const std::wstring& exe) {
    S::ClearExeCache();
    const S::GameCheck probe = [&] {
        S::Profile none{0, 0, "", ""};
        return S::CheckExe(L::Parent(exe), {none});
    }();
    return {S::Profile{probe.exe.size, probe.exe.timestamp, melange::hashutil::Sha256HexFile(exe), "Test #1"}};
}

void TestDetect() {
    const std::wstring root = Fresh(L"detect");
    const std::wstring steam = root + L"\\Steam", lib2 = root + L"\\Lib2", gog = root + L"\\GOG Games\\Worms";
    const std::wstring self = L::ExePath();
    // Steam root has no copy; library 2 has one (with appmanifest); GOG has one.
    auto esc = [](const std::wstring& p) {
        std::string out;
        for (char ch : L::Narrow(p)) {
            out += ch;
            if (ch == '\\') out += '\\';
        }
        return out;
    };
    Put(steam + L"\\steamapps\\libraryfolders.vdf",
        "\"libraryfolders\" { \"0\" { \"path\" \"" + esc(steam) + "\" } \"1\" { \"path\" \"" + esc(lib2) + "\" \"apps\" { \"70600\" \"1\" } } }");
    Put(lib2 + L"\\steamapps\\appmanifest_70600.acf", "\"AppState\" { \"appid\" \"70600\" \"installdir\" \"WormsXHD\" }");
    Copy(self, lib2 + L"\\steamapps\\common\\WormsXHD\\WormsMayhem.exe");
    Copy(self, gog + L"\\WormsMayhem.exe");
    const auto profiles = FakeProfiles(lib2 + L"\\steamapps\\common\\WormsXHD\\WormsMayhem.exe");

    FakeReg hkcu;
    std::wstring fwd = steam;
    for (auto& c : fwd)
        if (c == L'\\') c = L'/';
    hkcu.Set(S::Hive::CurrentUser, L"Software\\Valve\\Steam", L"SteamPath", fwd);
    hkcu.Set(S::Hive::LocalMachine, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1234", L"exe", gog + L"\\WormsMayhem.exe");
    hkcu.Set(S::Hive::LocalMachine, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1234", L"path", gog);
    S::DetectInput in;
    in.reg = &hkcu;
    in.profiles = &profiles;
    auto c = S::Detect(in);
    Expect(c.size() == 2, "detect: Steam (HKCU) library + GOG by exe", std::to_string(c.size()));
    if (c.size() == 2) {
        Expect(c[0].source == "steam" && c[0].check.verdict == S::Verdict::Ok && c[0].check.store == "steam", "detect: steam candidate ok");
        Expect(L::PathKey(c[0].library) == L::PathKey(lib2), "detect: steam candidate carries its library");
        Expect(c[1].source == "gog" && c[1].check.store == "gog", "detect: gog candidate");
    }
    // HKLM only, GOG by name, the uninstall key duplicating the Steam folder, a saved folder first.
    FakeReg hklm;
    hklm.Set(S::Hive::LocalMachine, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", steam);
    hklm.Set(S::Hive::LocalMachine, L"SOFTWARE\\GOG.com\\Games\\99", L"gameName", L"Worms Ultimate Mayhem");
    hklm.Set(S::Hive::LocalMachine, L"SOFTWARE\\GOG.com\\Games\\99", L"path", gog + L"\\");
    hklm.Set(S::Hive::LocalMachine, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 70600", L"InstallLocation",
             lib2 + L"\\steamapps\\common\\WORMSXHD");
    in.reg = &hklm;
    in.saved = gog;
    c = S::Detect(in);
    Expect(c.size() == 2 && c[0].source == "saved" && c[1].source == "steam", "detect: HKLM, saved first, de-duplicated by path",
           std::to_string(c.size()));
    // Both hives: HKCU wins; Melange.exe's own folder counts when the exe is there.
    FakeReg both = hkcu;
    both.Set(S::Hive::LocalMachine, L"SOFTWARE\\Valve\\Steam", L"InstallPath", root + L"\\Nowhere");
    in.reg = &both;
    in.saved.clear();
    in.selfDir = lib2 + L"\\steamapps\\common\\WormsXHD";
    c = S::Detect(in);
    Expect(!c.empty() && c[0].source == "self" && c[0].check.store == "steam", "detect: self source, store from the Steam library");
    Expect(c.size() == 2, "detect: self de-duplicates the Steam entry", std::to_string(c.size()));
    // No acf, but steamapps\common\WormsXHD exists.
    DeleteFileW((lib2 + L"\\steamapps\\appmanifest_70600.acf").c_str());
    in.selfDir.clear();
    c = S::Detect(in);
    Expect(!c.empty() && c[0].source == "steam", "detect: folder without appmanifest still a candidate");
    // A parent picked by mistake.
    Expect(L::PathKey(S::ChildWithGame(lib2 + L"\\steamapps\\common")) == L::PathKey(lib2 + L"\\steamapps\\common\\WormsXHD"),
           "browse: child hint from the common folder");
    Expect(L::PathKey(S::ChildWithGame(lib2)) == L::PathKey(lib2 + L"\\steamapps\\common\\WormsXHD"), "browse: child hint from a library root");
    Expect(S::ChildWithGame(gog).empty(), "browse: no hint when the folder has the exe");
    Expect(S::StoreOf(gog, hkcu) == "gog" && S::StoreOf(root, hkcu) == "unknown", "store of a folder");
}

// ---------------------------------------------------------------- exe check
void TestExe() {
    const std::wstring dir = Fresh(L"exe");
    const std::wstring exe = dir + L"\\WormsMayhem.exe";
    Copy(L::ExePath(), exe);
    auto profiles = FakeProfiles(exe);
    S::ClearExeCache();
    int before = S::g_hashCount;
    S::GameCheck c = S::CheckExe(dir, profiles);
    Expect(c.verdict == S::Verdict::Ok && c.exe.build == "Test #1", "exe: matching profile is ok");
    Expect(S::g_hashCount == before + 1, "exe: hashed once");
    before = S::g_hashCount;
    S::CheckExe(dir, profiles);
    Expect(S::g_hashCount == before, "exe: second check is cached");
    auto wrongSize = profiles;
    wrongSize[0].size += 1;
    S::ClearExeCache();
    before = S::g_hashCount;
    c = S::CheckExe(dir, wrongSize);
    Expect(c.verdict == S::Verdict::WrongBuild && S::g_hashCount == before, "exe: wrong size is wrongBuild without hashing");
    auto wrongHash = profiles;
    wrongHash[0].sha256 = std::string(64, '0');
    S::ClearExeCache();
    c = S::CheckExe(dir, wrongHash);
    Expect(c.verdict == S::Verdict::WrongBuild && !c.exe.sha256.empty(), "exe: same size, other hash is wrongBuild");
    Expect(S::CheckExe(dir + L"\\nope", profiles).verdict == S::Verdict::NotFound, "exe: missing folder is notFound");
    const std::wstring empty = Fresh(L"exe-empty");
    Expect(S::CheckExe(empty, profiles).verdict == S::Verdict::NoExe, "exe: no exe");
    S::ClearExeCache();
    HANDLE lock = CreateFileW(exe.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    c = S::CheckExe(dir, profiles);
    Expect(c.verdict == S::Verdict::Unreadable && !c.error.empty(), "exe: exclusively locked file is unreadable", c.error);
    CloseHandle(lock);
    Expect(S::DefaultProfiles().size() == 1 && S::DefaultProfiles()[0].sha256 == "041c8c6eb3b9f4fbaf367748f713ccb8f7bef68d13e825472c88c1ecf711ab7d",
           "exe: the shared #1077 profile");
}

// ---------------------------------------------------------------- loader identity
std::wstring Ual() { return g_src + L"\\tools\\ual\\dinput8.dll"; }

void TestDll() {
    S::DllInfo d;
    if (L::FileExists(Ual())) {
        Expect(S::IdentifyDll(Ual(), S::KnownUalHashes()[0], &d), "dll: UAL readable");
        Expect(d.kind == S::DllKind::Ual && d.known && d.ours, "dll: pinned UAL is ual, known, ours");
        Expect(d.product == "Ultimate-ASI-Loader-Win32" && d.description == "Ultimate ASI Loader", "dll: UAL version strings", d.product);
        Expect(S::Describe(d) == "Ultimate ASI Loader 9.7.4", "dll: UAL label", S::Describe(d));
        Expect(GetModuleHandleW(Ual().c_str()) == nullptr, "dll: UAL never loaded");
        // A different UAL build: same version strings, other bytes -> ual by resource, not ours.
        const std::wstring other = Fresh(L"dll") + L"\\dinput8.dll";
        std::string bytes = Get(Ual());
        bytes += "padding";
        Put(other, bytes);
        Expect(S::IdentifyDll(other, S::KnownUalHashes()[0], &d) && d.kind == S::DllKind::Ual && d.known && !d.ours, "dll: other UAL build");
    } else {
        Expect(false, "dll: tools\\ual\\dinput8.dll is missing (copy the pinned Ultimate ASI Loader there)");
    }
    Expect(S::IdentifyDll(g_bin + L"\\fake_dinput8_reshade.dll", "", &d) && d.kind == S::DllKind::ReShade && d.company == "crosire",
           "dll: ReShade by product name", S::DllKindName(d.kind));
    Expect(S::Describe(d) == "ReShade 6.3.0.0", "dll: ReShade label", S::Describe(d));
    Expect(d.exportsDinput, "dll: export table read from disk");
    Expect(S::IdentifyDll(g_bin + L"\\fake_dinput8_microsoft.dll", "", &d) && d.kind == S::DllKind::Microsoft, "dll: Microsoft company");
    Expect(S::IdentifyDll(g_bin + L"\\fake_dinput8_other.dll", "", &d) && d.kind == S::DllKind::Other && d.product.empty(), "dll: no version resource is other");
    Expect(GetModuleHandleW((g_bin + L"\\fake_dinput8_reshade.dll").c_str()) == nullptr &&
               GetModuleHandleW(L"fake_dinput8_reshade.dll") == nullptr,
           "dll: fixtures never loaded");
    Expect(!S::IdentifyDll(g_bin + L"\\nope.dll", "", &d), "dll: missing file");
    // The weak fallback: no resource, the UAL marker string, exports DirectInput8Create.
    const std::wstring weak = Fresh(L"dll-weak") + L"\\dinput8.dll";
    std::string bytes = Get(g_bin + L"\\fake_dinput8_other.dll");
    bytes += "Ultimate ASI Loader";
    Put(weak, bytes);
    Expect(S::IdentifyDll(weak, "", &d) && d.kind == S::DllKind::Ual && !d.known, "dll: weak UAL fallback");
}

// ---------------------------------------------------------------- engine
struct Rig {
    std::wstring game, payload, logs;
    std::unique_ptr<std::vector<S::Profile>> profiles;
    S::Context ctx;
};

const char* kTemplate =
    "; Melange settings\r\n"
    "[Oasis]\r\n"
    "; Serve the web app\r\n"
    "Enabled=1\r\n"
    "Port=8765\r\n"
    "\r\n"
    "; Logging\r\n"
    "[Logging]\r\n"
    "Enabled=1\r\n"
    "; New in this release\r\n"
    "Verbose=0\r\n";

Rig MakeRig(const wchar_t* name) {
    Rig r;
    const std::wstring root = Fresh(name);
    r.game = root + L"\\game";
    r.payload = root + L"\\payload";
    Copy(L::ExePath(), r.game + L"\\WormsMayhem.exe");
    Put(r.game + L"\\Data\\level.xom", "game data");
    Copy(g_bin + L"\\fake_asi_0_4_0.dll", r.payload + L"\\melange.asi");
    Copy(Ual(), r.payload + L"\\dinput8.dll");
    Put(r.payload + L"\\Melange.ini", kTemplate);
    Copy(g_bin + L"\\fake_dinput8_other.dll", r.payload + L"\\Melange.exe");
    r.profiles = std::make_unique<std::vector<S::Profile>>(FakeProfiles(r.game + L"\\WormsMayhem.exe"));
    r.ctx.gameDir = r.game;
    r.ctx.payloadDir = r.payload;
    r.ctx.selfExe = r.payload + L"\\Melange.exe";
    r.ctx.version = "0.4.0";
    r.ctx.profiles = r.profiles.get();
    r.ctx.running = [](const std::wstring&) { return false; };
    r.ctx.loaded = [](const std::wstring&) { return false; };
    return r;
}

bool HasStep(const S::Plan& p, const char* op, const char* path) {
    for (const auto& s : p.steps)
        if (s.op == op && L::IEquals(s.path, path)) return true;
    return false;
}

std::string PlanText(const S::Plan& p) {
    std::string t;
    for (const auto& s : p.steps) t += s.op + " " + s.path + "; ";
    return t + (p.refused.empty() ? "" : " refused: " + p.refused);
}

std::string InstallJsonField(const std::wstring& game, const char* key) {
    melange::json::Value v;
    melange::json::Error e;
    if (!melange::json::ParseFile(game + L"\\Melange\\install.json", &v, &e)) return "";
    const auto* x = v.Get(key);
    return x && x->IsString() ? x->string : "";
}

void TestEngineFresh() {
    Rig r = MakeRig(L"eng-fresh");
    S::Status st = S::Inspect(r.ctx);
    Expect(st.payload.ok, "engine: payload ok", st.payload.missing.empty() ? "" : st.payload.missing[0]);
    Expect(st.loaderState == "none" && st.melangeState == "missing" && !st.iniPresent, "engine: fresh folder status");
    S::Plan p = S::MakePlan(r.ctx, {});
    Expect(p.refused.empty() && p.needsChoice.empty(), "engine: fresh plan runs", PlanText(p));
    Expect(HasStep(p, "add", "melange.asi") && HasStep(p, "add", "dinput8.dll") && HasStep(p, "add", "Melange.ini") && HasStep(p, "add", "Melange.exe"),
           "engine: fresh plan adds everything", PlanText(p));
    Expect(!p.steps.empty() && L::IEquals(p.steps.back().path, "dinput8.dll"), "engine: loader last on install");
    S::Outcome o = S::Apply(r.ctx, {}, p.planId);
    Expect(o.ok, "engine: fresh install", o.message);
    Expect(Get(r.game + L"\\melange.asi") == Get(r.payload + L"\\melange.asi") && Get(r.game + L"\\dinput8.dll") == Get(Ual()) &&
               Get(r.game + L"\\Melange.ini") == kTemplate,
           "engine: files in place");
    Expect(InstallJsonField(r.game, "loader") == "added" && InstallJsonField(r.game, "melange") == "0.4.0", "engine: install record");
    Expect(!L::DirExists(r.game + L"\\Melange\\.staging"), "engine: staging cleaned");
    st = S::Inspect(r.ctx);
    Expect(st.melangeState == "installed" && st.loaderState == "ual" && st.loader.ours, "engine: status after install", st.melangeState);
    p = S::MakePlan(r.ctx, {"repair"});
    bool allKeep = true;
    for (const auto& s : p.steps) allKeep &= s.op == "keep";
    Expect(allKeep, "engine: repair of a good install changes nothing", PlanText(p));
    Expect(S::Apply(r.ctx, {}, "stale").code == -32013, "engine: plan id mismatch");

    // Repair after melange.asi was deleted.
    DeleteFileW((r.game + L"\\melange.asi").c_str());
    p = S::MakePlan(r.ctx, {"repair"});
    Expect(HasStep(p, "add", "melange.asi"), "engine: repair re-adds melange.asi", PlanText(p));
    o = S::Apply(r.ctx, {"repair"}, p.planId);
    Expect(o.ok && L::FileExists(r.game + L"\\melange.asi"), "engine: repair applied", o.message);
    Expect(InstallJsonField(r.game, "loader") == "added", "engine: repair keeps loader=added");

    // Disable / enable.
    o = S::SetMelangeEnabled(r.ctx, false);
    Expect(o.ok && L::FileExists(r.game + L"\\melange.asi.off") && !L::FileExists(r.game + L"\\melange.asi"), "engine: disable");
    Expect(S::Inspect(r.ctx).melangeState == "disabled", "engine: disabled state");
    o = S::SetMelangeEnabled(r.ctx, true);
    Expect(o.ok && L::FileExists(r.game + L"\\melange.asi"), "engine: enable");

    // Uninstall, keeping data; then with data.
    Put(r.game + L"\\Mods\\mine\\spice.json", "{}");
    Put(r.game + L"\\Mods\\fromstore\\spice.json", "{}");
    Put(r.game + L"\\Mods\\.store\\installed.json", "{\"_serialSeen\":3,\"fromstore\":{\"version\":\"1.0.0\",\"sha256\":\"00\",\"serial\":3}}");
    Put(r.game + L"\\Melange\\logs\\x.log", "log");
    p = S::MakePlan(r.ctx, {"uninstall"});
    Expect(!p.steps.empty() && L::IEquals(p.steps.front().path, "dinput8.dll") && p.steps.front().op == "remove", "engine: uninstall removes the loader first",
           PlanText(p));
    o = S::Apply(r.ctx, {"uninstall"}, p.planId);
    Expect(o.ok && !L::FileExists(r.game + L"\\melange.asi") && !L::FileExists(r.game + L"\\dinput8.dll") && !L::FileExists(r.game + L"\\Melange.exe"),
           "engine: uninstall removed Melange", o.message);
    Expect(L::FileExists(r.game + L"\\Melange.ini") && L::DirExists(r.game + L"\\Mods\\fromstore") && L::FileExists(r.game + L"\\Melange\\logs\\x.log"),
           "engine: uninstall keeps data by default");
    Expect(!L::FileExists(r.game + L"\\Melange\\install.json"), "engine: uninstall drops the install record");
    S::Apply(r.ctx, {}, "");
    S::PlanRequest withData{"uninstall"};
    withData.removeData = true;
    o = S::Apply(r.ctx, withData, "");
    Expect(o.ok && !L::FileExists(r.game + L"\\Melange.ini") && !L::DirExists(r.game + L"\\Mods\\fromstore") && !L::DirExists(r.game + L"\\Mods\\.store") &&
               !L::DirExists(r.game + L"\\Melange\\logs"),
           "engine: uninstall with data", o.message);
    Expect(L::DirExists(r.game + L"\\Mods\\mine") && L::FileExists(r.game + L"\\Data\\level.xom") && L::FileExists(r.game + L"\\WormsMayhem.exe"),
           "engine: user mods and game files never touched");
    Expect(L::DirExists(r.game + L"\\Melange\\backup") && !S::ListBackups(r.game).empty(), "engine: backups survive uninstall with data");
}

void TestEngineLoaders() {
    // An existing UAL is reused.
    {
        Rig r = MakeRig(L"eng-ual");
        Copy(Ual(), r.game + L"\\dinput8.dll");
        S::Plan p = S::MakePlan(r.ctx, {});
        Expect(HasStep(p, "keep", "dinput8.dll") && !HasStep(p, "add", "dinput8.dll"), "engine: UAL reused", PlanText(p));
        Expect(S::Apply(r.ctx, {}, p.planId).ok && InstallJsonField(r.game, "loader") == "reused", "engine: reused recorded");
        p = S::MakePlan(r.ctx, {"uninstall"});
        Expect(HasStep(p, "keep", "dinput8.dll"), "engine: uninstall keeps a reused loader", PlanText(p));
    }
    // UAL as version.dll: no dinput8.dll is added.
    {
        Rig r = MakeRig(L"eng-alt");
        Copy(Ual(), r.game + L"\\version.dll");
        S::Status st = S::Inspect(r.ctx);
        Expect(st.otherLoaders.size() == 1 && st.otherLoaders[0].file == "version.dll", "engine: UAL under another name detected");
        S::Plan p = S::MakePlan(r.ctx, {});
        Expect(!HasStep(p, "add", "dinput8.dll") && HasStep(p, "keep", "version.dll"), "engine: other-name loader reused", PlanText(p));
        Expect(S::Apply(r.ctx, {}, p.planId).ok && !L::FileExists(r.game + L"\\dinput8.dll") && InstallJsonField(r.game, "loader") == "other-name",
               "engine: other-name recorded");
    }
    // Another program's dinput8.dll: a choice, then backup + replace, then restore.
    {
        Rig r = MakeRig(L"eng-reshade");
        Copy(g_bin + L"\\fake_dinput8_reshade.dll", r.game + L"\\dinput8.dll");
        const std::string original = Get(r.game + L"\\dinput8.dll");
        Put(r.game + L"\\Melange.ini", "[Oasis]\r\nPort=9000\r\n");
        const auto before = Snap(r.game);
        S::Plan p = S::MakePlan(r.ctx, {});
        Expect(p.needsChoice == "loader", "engine: other loader needs a choice", PlanText(p));
        S::Outcome o = S::Apply(r.ctx, {}, p.planId);
        Expect(!o.ok && o.code == -32000 && Snap(r.game) == before, "engine: refused without the choice, nothing written");
        S::PlanRequest rq;
        rq.replaceLoader = true;
        p = S::MakePlan(r.ctx, rq);
        Expect(HasStep(p, "backup", "dinput8.dll") && HasStep(p, "replace", "dinput8.dll") && HasStep(p, "merge", "Melange.ini"),
               "engine: replace plan", PlanText(p));
        o = S::Apply(r.ctx, rq, p.planId);
        Expect(o.ok && !o.backupId.empty(), "engine: replaced with a backup", o.message);
        Expect(Get(r.game + L"\\dinput8.dll") == Get(Ual()), "engine: UAL in place");
        const std::wstring bdir = r.game + L"\\Melange\\backup\\" + L::Widen(o.backupId);
        Expect(Get(bdir + L"\\dinput8.dll") == original, "engine: original in the backup");
        const std::string manifest = Get(bdir + L"\\manifest.json");
        Expect(manifest.find("\"kind\":\"reshade\"") != std::string::npos && manifest.find("\"op\":\"replaced\"") != std::string::npos &&
                   manifest.find("\"op\":\"added\"") != std::string::npos,
               "engine: manifest records kinds and ops", manifest);
        const std::string ini = Get(r.game + L"\\Melange.ini");
        Expect(ini.rfind("[Oasis]\r\nPort=9000\r\n", 0) == 0 && ini.find("Enabled=1") != std::string::npos, "engine: ini merged, user value kept", ini);
        Expect(InstallJsonField(r.game, "loader") == "replaced", "engine: replaced recorded");
        auto backups = S::ListBackups(r.game);
        Expect(backups.size() == 1 && backups[0].action == "install", "engine: backup listed");
        // Restore: originals byte-equal, added files gone.
        o = S::Restore(r.ctx, backups[0].id);
        Expect(o.ok, "engine: restore", o.message);
        Expect(Get(r.game + L"\\dinput8.dll") == original && Get(r.game + L"\\Melange.ini") == "[Oasis]\r\nPort=9000\r\n", "engine: restore byte-equal");
        Expect(!L::FileExists(r.game + L"\\melange.asi"), "engine: restore deletes added files");
        Expect(S::ListBackups(r.game).size() == 2, "engine: restore is itself backed up");
        // Uninstall after a replace puts the original loader back.
        rq.replaceLoader = true;
        o = S::Apply(r.ctx, rq, "");
        Expect(o.ok && Get(r.game + L"\\dinput8.dll") == Get(Ual()), "engine: replaced again");
        o = S::Apply(r.ctx, {"uninstall"}, "");
        Expect(o.ok && Get(r.game + L"\\dinput8.dll") == original, "engine: uninstall restores the replaced loader", o.message);
        const std::string id = S::ListBackups(r.game).back().id;
        Expect(S::DeleteBackup(r.ctx, id).ok && !L::DirExists(r.game + L"\\Melange\\backup\\" + L::Widen(id)), "engine: delete backup");
        Expect(S::DeleteBackup(r.ctx, "..\\..").code == -32602, "engine: backup ids are checked");
    }
}

void TestEngineCleanup() {
    Rig r = MakeRig(L"eng-clean");
    Copy(g_bin + L"\\fake_asi_0_3_1.dll", r.game + L"\\melange.asi");
    Copy(g_bin + L"\\fake_asi_0_3_1.dll", r.game + L"\\scripts\\melange.asi");
    Put(r.game + L"\\WUMFix.asi", "old");
    Put(r.game + L"\\oasis.exe", "old");
    S::Status st = S::Inspect(r.ctx);
    Expect(st.melangeState == "older" && st.duplicates.size() == 1 && st.legacy.size() == 2, "engine: older, duplicate and legacy detected");
    S::Plan p = S::MakePlan(r.ctx, {"repair"});
    Expect(HasStep(p, "replace", "melange.asi") && HasStep(p, "remove", "scripts\\melange.asi") && HasStep(p, "remove", "WUMFix.asi") &&
               HasStep(p, "remove", "oasis.exe"),
           "engine: repair plan cleans up", PlanText(p));
    S::Outcome o = S::Apply(r.ctx, {"repair"}, p.planId);
    Expect(o.ok && !L::FileExists(r.game + L"\\scripts\\melange.asi") && !L::FileExists(r.game + L"\\WUMFix.asi") && !L::FileExists(r.game + L"\\oasis.exe"),
           "engine: cleaned", o.message);
    Expect(L::FileExists(r.game + L"\\Melange\\backup\\" + L::Widen(o.backupId) + L"\\WUMFix.asi"), "engine: legacy backed up");
    Expect(S::Inspect(r.ctx).melangeState == "installed", "engine: updated");

    // A newer melange.asi is kept unless a downgrade is allowed.
    Copy(g_bin + L"\\fake_asi_9_0_0.dll", r.game + L"\\melange.asi");
    Expect(S::Inspect(r.ctx).melangeState == "newer", "engine: newer state");
    p = S::MakePlan(r.ctx, {});
    Expect(HasStep(p, "keep", "melange.asi"), "engine: newer kept", PlanText(p));
    S::PlanRequest down;
    down.allowDowngrade = true;
    p = S::MakePlan(r.ctx, down);
    Expect(HasStep(p, "replace", "melange.asi"), "engine: downgrade when allowed", PlanText(p));

    // Payload missing.
    Rig m = MakeRig(L"eng-nopayload");
    DeleteFileW((m.payload + L"\\melange.asi").c_str());
    p = S::MakePlan(m.ctx, {});
    o = S::Apply(m.ctx, {}, "");
    Expect(!o.ok && o.code == -32011 && !o.missing.empty(), "engine: payload missing is -32011", o.message);
    Copy(g_bin + L"\\fake_asi_0_3_1.dll", m.payload + L"\\melange.asi");
    Expect(!S::Inspect(m.ctx).payload.ok, "engine: payload version must match");
}

void TestEngineGuards() {
    Rig r = MakeRig(L"eng-guard");
    const auto before = Snap(r.game);
    // Wrong build.
    auto bad = *r.profiles;
    bad[0].sha256 = std::string(64, 'f');
    S::ClearExeCache();
    S::Context c = r.ctx;
    c.profiles = &bad;
    S::Outcome o = S::Apply(c, {}, "");
    Expect(!o.ok && o.code == -32000 && o.message.find("build #1077") != std::string::npos, "guard: wrong build refused", o.message);
    S::ClearExeCache();
    // Game running: a real Melange mutex for this folder.
    std::wstring key = L::LowerW(L::FullPath(r.game));
    uint32_t h = 2166136261u;
    for (wchar_t ch : key) {
        h ^= static_cast<uint32_t>(ch);
        h *= 16777619u;
    }
    wchar_t name[64];
    swprintf(name, 64, L"Local\\Melange-%08x", h);
    HANDLE mx = CreateMutexW(nullptr, FALSE, name);
    Expect(S::GameRunning(r.game) && S::MelangeLoaded(r.game), "guard: the mutex is seen");
    c = r.ctx;
    c.running = nullptr;
    o = S::Apply(c, {}, "");
    Expect(!o.ok && o.message == "Close Worms Ultimate Mayhem first.", "guard: game running refused", o.message);
    Expect(!S::SetMelangeEnabled(c, false).ok, "guard: disable refused while running");
    CloseHandle(mx);
    Expect(!S::GameRunning(r.game), "guard: mutex released");
    // MELANGE_PROTECT.
    SetEnvironmentVariableW(L"MELANGE_PROTECT", (L"C:\\nowhere;" + r.game + L"\\").c_str());
    c = r.ctx;
    c.protect = S::ProtectFromEnv();
    o = S::Apply(c, {}, "");
    Expect(!o.ok && o.message.find("MELANGE_PROTECT") != std::string::npos, "guard: protected folder refused", o.message);
    Expect(!S::DeleteBackup(c, "x").ok, "guard: protected delete refused");
    SetEnvironmentVariableW(L"MELANGE_PROTECT", nullptr);
    Expect(Snap(r.game) == before, "guard: nothing written by refused calls");
    // Access denied while committing.
    c = r.ctx;
    c.move = [](const std::wstring&, const std::wstring&) -> unsigned long { return ERROR_ACCESS_DENIED; };
    o = S::Apply(c, {}, "");
    Expect(!o.ok && o.code == -32010, "guard: access denied is -32010", std::to_string(o.code));
    Expect(Snap(r.game, L"Melange") == before, "guard: access denied leaves the folder as it was");
}

void TestEngineRollback() {
    // Fail the n-th rename for every n; the folder must be byte-identical afterwards.
    for (int failAt = 1; failAt <= 12; ++failAt) {
        Rig r = MakeRig(L"eng-rollback");
        Copy(g_bin + L"\\fake_dinput8_reshade.dll", r.game + L"\\dinput8.dll");
        Copy(g_bin + L"\\fake_asi_0_3_1.dll", r.game + L"\\melange.asi");
        Put(r.game + L"\\WUMFix.asi", "old");
        Put(r.game + L"\\Melange.ini", "[Oasis]\nPort=1\n");
        const auto before = Snap(r.game, L"Melange");
        int calls = 0;
        S::Context c = r.ctx;
        c.move = [&](const std::wstring& a, const std::wstring& b) -> unsigned long {
            if (++calls == failAt) return ERROR_SHARING_VIOLATION;
            return S::DefaultMove(a, b);
        };
        S::PlanRequest rq;
        rq.replaceLoader = true;
        S::Outcome o = S::Apply(c, rq, "");
        if (calls < failAt) {
            Expect(o.ok, "rollback: no failure injected -> ok");
            break;
        }
        char what[64];
        snprintf(what, sizeof what, "rollback: failure at rename %d", failAt);
        Expect(!o.ok && o.code == -32012 && !o.failedPath.empty() && o.win32 == ERROR_SHARING_VIOLATION, what, o.message);
        Expect(Snap(r.game, L"Melange") == before, what, "folder changed");
        Expect(S::ListBackups(r.game).empty(), what, "a backup was left behind");
    }
}

// ---------------------------------------------------------------- ini merge
void TestIniMerge() {
    const std::string user =
        "; my notes\r\n"
        "[Oasis]\r\n"
        "Port=9999   ; mine\r\n"
        "Custom=yes\r\n"
        "\r\n"
        "[Mod.sunstone]\r\n"
        "quality=ultra\r\n";
    int added = 0;
    const std::string m = S::IniMerge(kTemplate, user, &added);
    Expect(added == 3, "ini: three keys added", std::to_string(added));
    Expect(m.find("Port=9999   ; mine\r\n") != std::string::npos && m.find("Custom=yes\r\n") != std::string::npos &&
               m.find("quality=ultra\r\n") != std::string::npos && m.rfind("; my notes\r\n[Oasis]\r\nPort=9999", 0) == 0,
           "ini: user bytes kept", m);
    Expect(m.find("Custom=yes\r\n; Serve the web app\r\nEnabled=1\r\n") != std::string::npos, "ini: missing key after the section's last key, with its comment", m);
    Expect(m.find("; Logging\r\n[Logging]\r\nEnabled=1\r\n; New in this release\r\nVerbose=0\r\n") != std::string::npos, "ini: missing section appended with comments", m);
    Expect(m.find("Port=8765") == std::string::npos, "ini: user value not duplicated");
    Expect(S::IniMerge(kTemplate, m, &added) == m && added == 0, "ini: idempotent");
    Expect(S::IniMissingKeys(kTemplate, user) == 3 && S::IniMissingKeys(kTemplate, m) == 0, "ini: missing key count");
    const std::string lf = "[Oasis]\nEnabled=0\n";
    const std::string mlf = S::IniMerge(kTemplate, lf, &added);
    Expect(mlf.find("\r\n") == std::string::npos && mlf.rfind("[Oasis]\nEnabled=0\nPort=8765\n", 0) == 0, "ini: LF file stays LF", mlf);
    const std::string noEol = "[oasis]\r\nenabled=1";
    const std::string mno = S::IniMerge(kTemplate, noEol, &added);
    Expect(mno.rfind("[oasis]\r\nenabled=1\r\nPort=8765", 0) == 0, "ini: case-insensitive sections and keys, last line without EOL", mno);
    Expect(S::IniMerge(kTemplate, "", &added) == kTemplate, "ini: empty user file gets the template");
}

// ---------------------------------------------------------------- plugin settings
void TestPluginSettings() {
    const std::wstring game = Fresh(L"plugins");
    Put(game + L"\\Mods\\sunstone\\spice.json",
        "{\"spiceVersion\":1,\"id\":\"sunstone\",\"version\":\"1.7.0\",\"name\":\"Sunstone\",\"settings\":["
        "{\"key\":\"quality\",\"type\":\"enum\",\"options\":[\"off\",\"low\",\"subtle\",\"bold\",\"ultra\"],\"default\":\"bold\",\"label\":\"Quality\","
        "\"help\":\"How much Sunstone changes.\",\"optionLabels\":{\"ultra\":{\"label\":\"Ultra\",\"help\":\"Needs a strong GPU.\"}}},"
        "{\"key\":\"water\",\"type\":\"bool\",\"default\":true,\"label\":\"Water\"},"
        "{\"key\":\"count\",\"type\":\"int\",\"default\":3,\"min\":1,\"max\":8,\"label\":\"Count\"},"
        "{\"key\":\"gain\",\"type\":\"float\",\"default\":0.5,\"min\":0,\"max\":1,\"label\":\"Gain\"},"
        "{\"key\":\"name\",\"type\":\"string\",\"default\":\"x\",\"label\":\"Name\"}]}");
    Put(game + L"\\Melange.ini", "[Oasis]\r\nEnabled=1\r\n");
    std::vector<L::plugins::Setting> decl;
    std::string err;
    Expect(L::plugins::LoadDecl(game, "sunstone", &decl, &err) && decl.size() == 5, "settings: declaration read", err);
    if (decl.size() == 5) {
        Expect(decl[0].help == "How much Sunstone changes." && decl[0].optionLabels.size() == 1 && decl[0].optionLabels[0].help == "Needs a strong GPU.",
               "settings: help and optionLabels");
        Expect(decl[2].hasMin && decl[2].hasMax && decl[2].max == 8, "settings: ranges");
    }
    auto vals = L::plugins::ReadValues(game, "sunstone", decl);
    Expect(vals["quality"].str == "bold" && vals["water"].b && vals["count"].num == 3, "settings: defaults when unset");
    std::string key, why;
    std::map<std::string, L::plugins::Val> in;
    in["quality"] = L::plugins::Val::S("ultra");
    in["count"] = L::plugins::Val::N(5);
    in["water"] = L::plugins::Val::B(false);
    in["gain"] = L::plugins::Val::N(0.25);
    Expect(L::plugins::Validate(decl, in, &key, &why), "settings: valid values", key + ": " + why);
    Expect(L::plugins::WriteValues(game, "sunstone", decl, in, &err), "settings: written", err);
    const std::string ini = Get(game + L"\\Melange.ini");
    Expect(ini.rfind("[Oasis]\r\nEnabled=1\r\n", 0) == 0 && ini.find("[Mod.sunstone]") != std::string::npos && ini.find("quality=ultra") != std::string::npos &&
               ini.find("water=false") != std::string::npos && ini.find("count=5") != std::string::npos && ini.find("gain=0.25") != std::string::npos,
           "settings: stored in [Mod.<id>]", ini);
    vals = L::plugins::ReadValues(game, "sunstone", decl);
    Expect(vals["quality"].str == "ultra" && !vals["water"].b && vals["count"].num == 5 && vals["gain"].num == 0.25, "settings: round trip");
    auto bad = [&](const char* k, L::plugins::Val v, const char* what) {
        std::map<std::string, L::plugins::Val> one{{k, v}};
        std::string bk, bw;
        Expect(!L::plugins::Validate(decl, one, &bk, &bw) && bk == k, what, bw);
    };
    bad("quality", L::plugins::Val::S("mega"), "settings: enum membership");
    bad("quality", L::plugins::Val::N(1), "settings: enum type");
    bad("count", L::plugins::Val::N(9), "settings: int max");
    bad("count", L::plugins::Val::N(2.5), "settings: int must be whole");
    bad("gain", L::plugins::Val::N(-0.1), "settings: float min");
    bad("water", L::plugins::Val::S("yes"), "settings: bool type");
    bad("name", L::plugins::Val::S("a\nb"), "settings: no line breaks");
    bad("name", L::plugins::Val::S("a;b"), "settings: no ';'");
    bad("nope", L::plugins::Val::S("x"), "settings: unknown key");
}

// ---------------------------------------------------------------- recommended set, store index
void TestRecommended() {
    const std::string index =
        "{\"indexVersion\":1,\"serial\":4,\"recommended\":[{\"id\":\"sunstone\",\"settings\":{\"quality\":\"bold\"},\"why\":\"Sharper graphics.\"},"
        "{\"id\":\"ghost\",\"settings\":{}}],"
        "\"plugins\":[{\"id\":\"sunstone\",\"name\":\"Sunstone\",\"authors\":[\"Melange\"],\"description\":\"Graphics.\",\"licence\":\"MIT\","
        "\"categories\":[\"graphics\"],\"gameBuilds\":[\"1077\"],\"versions\":[{\"version\":\"1.7.0\",\"released\":\"2026-10-03\",\"melange\":\">=0.3.0\","
        "\"kind\":\"client-only\",\"permissions\":{\"unsafe\":false,\"filesystem\":\"none\"},\"url\":\"sunstone-1.7.0.zip\","
        "\"sha256\":\"7d06d52b7c2f1ff119cd9b56399e5b0d048e2deb6d246ac9218491686dbfbe03\",\"size\":10,\"unpackedSize\":10,\"files\":1}]}]}";
    melange::store::Index idx;
    std::string err;
    Expect(melange::store::ParseIndex(index, &idx, &err) && idx.plugins.size() == 1, "store: an unknown root key 'recommended' is accepted", err);
    std::vector<L::Recommended> rec;
    Expect(L::ParseRecommended(index, idx, &rec), "recommended: parsed from the index");
    Expect(rec.size() == 1 && rec[0].id == "sunstone" && rec[0].why == "Sharper graphics." && rec[0].settings.count("quality"),
           "recommended: unknown ids dropped", std::to_string(rec.size()));
    const std::string badSetting = "{\"recommended\":[{\"id\":\"sunstone\",\"settings\":{\"quality\":\"mega\"}}]}";
    Expect(L::ParseRecommended(badSetting, idx, &rec) && rec.empty(), "recommended: settings must match the declaration");
    auto builtin = L::BuiltinRecommended();
    Expect(builtin.size() == 1 && builtin[0].id == "sunstone" && builtin[0].settings.at("quality").str == "bold", "recommended: built-in Sunstone on Bold");
}

// ---------------------------------------------------------------- the Store engine on a non-game host
class FakeStoreHost final : public melange::store::Host {
  public:
    std::wstring mods;
    std::string gate;
    std::vector<std::string> calls;
    std::string MelangeVersion() override { return "0.4.0"; }
    std::string GameBuild() override { return "1077"; }
    std::string Gate() override { return gate; }
    std::vector<melange::store::LocalMod> InstalledMods() override {
        std::vector<melange::store::LocalMod> out;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((mods + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            melange::json::Value v;
            melange::json::Error e;
            if (!melange::json::ParseFile(mods + L"\\" + fd.cFileName + L"\\spice.json", &v, &e)) continue;
            melange::store::LocalMod m;
            m.id = v.Get("id") ? v.Get("id")->string : "";
            m.version = v.Get("version") ? v.Get("version")->string : "";
            m.state = "enabled";
            m.enabled = true;
            out.push_back(m);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return out;
    }
    void Placed(const std::string& id, bool enable) override { calls.push_back("placed " + id + (enable ? " on" : " off")); }
    void Unload(const std::string& id) override { calls.push_back("unload " + id); }
    void Reload(const std::string& id, bool enable) override { calls.push_back("reload " + id + (enable ? " on" : " off")); }
    void Forget(const std::string& id) override { calls.push_back("forget " + id); }
    void DeleteData(const std::string& id) override { calls.push_back("delete " + id); }
};

std::string MakePluginZip(const std::string& version, uint64_t* unpacked) {
    const std::string spice = "{\"spiceVersion\":1,\"id\":\"hello\",\"version\":\"" + version +
                              "\",\"name\":\"Hello\",\"authors\":[\"me\"],\"melange\":{\"range\":\">=0.1.0\"},\"kind\":\"client-only\","
                              "\"entry\":{\"client\":\"client/init.lua\"},\"settings\":[{\"key\":\"level\",\"type\":\"int\",\"default\":2,"
                              "\"min\":1,\"max\":5,\"label\":\"Level\"}]}";
    const std::pair<std::string, std::string> files[] = {
        {"hello/spice.json", spice}, {"hello/LICENSE", "MIT"}, {"hello/client/init.lua", "wum.log.info('hello')"}};
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 0);
    *unpacked = 0;
    for (const auto& [name, data] : files) {
        mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);
        *unpacked += data.size();
    }
    void* buf = nullptr;
    size_t n = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &n);
    std::string out(static_cast<const char*>(buf), n);
    mz_zip_writer_end(&z);
    return out;
}

template <class F>
bool WaitFor(F done, int ms = 15000) {
    for (int i = 0; i < ms / 50; ++i) {
        if (done()) return true;
        Sleep(50);
    }
    return done();
}

void TestStoreEngine() {
    namespace st = melange::store;
    const std::wstring root = Fresh(L"store");
    const std::wstring game = root + L"\\game";
    L::MakeDirs(game + L"\\Mods");
    uint64_t unpacked = 0;
    const std::string zip = MakePluginZip("1.0.0", &unpacked);
    Put(root + L"\\index\\hello-1.0.0.zip", zip);
    const std::string sha = melange::hashutil::Sha256Hex(zip.data(), zip.size());
    Put(root + L"\\index\\index.json",
        "{\"indexVersion\":1,\"serial\":1,\"recommended\":[{\"id\":\"hello\",\"settings\":{\"level\":4}}],\"plugins\":[{\"id\":\"hello\","
        "\"name\":\"Hello\",\"authors\":[\"me\"],\"description\":\"A test plugin.\",\"licence\":\"MIT\",\"categories\":[\"misc\"],"
        "\"gameBuilds\":[\"1077\"],\"versions\":[{\"version\":\"1.0.0\",\"released\":\"2026-10-03\",\"melange\":\">=0.1.0\","
        "\"kind\":\"client-only\",\"permissions\":{\"unsafe\":false,\"filesystem\":\"none\"},\"url\":\"hello-1.0.0.zip\",\"sha256\":\"" +
            sha + "\",\"size\":" + std::to_string(zip.size()) + ",\"unpackedSize\":" + std::to_string(unpacked) + ",\"files\":3}]}]}");
    std::string url = "file:///" + L::Narrow(root + L"\\index\\index.json");
    for (char& c : url)
        if (c == '\\') c = '/';
    FakeStoreHost host;
    host.mods = game + L"\\Mods";
    st::Config cfg;
    cfg.indexUrl = url;
    cfg.custom = true;
    st::SetHost(&host, cfg);
    Expect(st::Open(game + L"\\Mods"), "store: opened on a Mods folder");
    Expect(st::Active(), "store: active with a host");
    st::Refresh();
    Expect(WaitFor([] { return st::GetStatus().haveIndex && !st::GetStatus().fetching; }), "store: file:// index fetched", st::GetStatus().error);
    Expect(st::IndexText().find("\"recommended\"") != std::string::npos, "store: index text kept for the recommended list");
    std::vector<L::Recommended> rec;
    melange::store::Index idx;
    std::string err;
    Expect(st::ParseIndex(st::IndexText(), &idx, &err) &&
               L::ParseRecommended(st::IndexText(), idx, &rec, [](const std::string&, std::vector<L::plugins::Setting>*) { return false; }) &&
               rec.size() == 1 && rec[0].name == "Hello",
           "store: recommended list from the fetched index");
    host.gate = "Close Worms Ultimate Mayhem first.";
    Expect(st::Install("hello", "", true, false).code == -32000, "store: the host's gate refuses installs");
    host.gate.clear();
    const st::Outcome o = st::Install("hello", "", true, false);
    Expect(o.code == 0, "store: install started", o.message);
    Expect(WaitFor([] { return !st::GetStatus().busy; }), "store: install finished");
    Expect(L::FileExists(game + L"\\Mods\\hello\\spice.json") && L::FileExists(game + L"\\Mods\\.store\\installed.json"), "store: plugin placed and recorded",
           st::GetStatus().job.message);
    Expect(!host.calls.empty() && host.calls.back() == "placed hello on", "store: host told the plugin is in place",
           host.calls.empty() ? "" : host.calls.back());
    auto items = st::List(st::ListQuery{});
    Expect(items.size() == 1 && items[0].installed && items[0].managed, "store: listed as installed from the Store");
    std::vector<L::plugins::Setting> decl;
    Expect(L::plugins::LoadDecl(game, "hello", &decl, &err) && decl.size() == 1 && decl[0].def.num == 2, "store: installed plugin's settings readable");
    Expect(st::Remove("hello", true).code == 0, "store: remove started");
    Expect(WaitFor([] { return !st::GetStatus().busy; }), "store: remove finished");
    Expect(!L::DirExists(game + L"\\Mods\\hello"), "store: plugin removed");
    bool forgot = false, deleted = false, unloaded = false;
    for (const auto& c : host.calls) {
        forgot |= c == "forget hello";
        deleted |= c == "delete hello";
        unloaded |= c == "unload hello";
    }
    Expect(unloaded && forgot && deleted, "store: host unloaded, forgot and deleted the plugin's data");
    st::Close();
    Expect(!st::Active(), "store: closed without a folder");
}

// ---------------------------------------------------------------- launcher.json
void TestSettings() {
    const std::wstring dir = Fresh(L"settings");
    L::Settings s;
    s.gameDir = L"D:\\Games\\Worms \"X\"";
    s.firstRunDone = true;
    s.theme = "dark";
    L::DefaultPlugin d;
    d.id = "sunstone";
    d.enabled = true;
    d.settings["quality"] = L::plugins::Val::S("bold");
    d.settings["water"] = L::plugins::Val::B(true);
    s.defaults.push_back(d);
    s.defaultsSeeded = true;
    Expect(L::SaveSettings(dir + L"\\launcher.json", s), "launcher.json: saved");
    L::Settings t;
    Expect(L::LoadSettings(dir + L"\\launcher.json", &t), "launcher.json: loaded");
    Expect(t.gameDir == s.gameDir && t.firstRunDone && t.theme == "dark" && t.defaultsSeeded && t.defaults.size() == 1 &&
               t.defaults[0].settings["quality"].str == "bold" && t.defaults[0].settings["water"].b,
           "launcher.json: round trip");
    Put(dir + L"\\bad.json", "{not json");
    Expect(!L::LoadSettings(dir + L"\\bad.json", &t) && t.gameDir.empty() && t.theme == "system", "launcher.json: bad file -> defaults");
}

// A recommended-plugins batch (or anything else holding app::Tx() for longer than one RPC call) is set on
// setup::Status by the app layer, after Inspect(); StatusJson() must round-trip it so setup.status can show it.
void TestBusyStatusJson() {
    S::Status s;
    Expect(S::StatusJson(s).find("\"busy\"") == std::string::npos, "status: no busy field when nothing is running");

    s.busyActive = true;
    s.busyAction = "recommended";
    s.busyStep = 1;
    s.busyOf = 2;
    s.busyLabel = "Installing Sunstone\xE2\x80\xA6";   // UTF-8 ellipsis, as the real label uses
    const std::string json = S::StatusJson(s);
    melange::json::Value v;
    melange::json::Error e;
    Expect(melange::json::Parse(json, &v, &e), "status: busy JSON parses", e.text);
    const melange::json::Value* busy = v.Get("busy");
    Expect(busy && busy->IsObject(), "status: busy is an object");
    if (busy) {
        Expect(busy->Get("action") && busy->Get("action")->string == "recommended", "status: busy.action");
        Expect(busy->Get("step") && busy->Get("step")->IsNumber() && busy->Get("step")->number == 1, "status: busy.step");
        Expect(busy->Get("of") && busy->Get("of")->number == 2, "status: busy.of");
        Expect(busy->Get("label") && busy->Get("label")->string == s.busyLabel, "status: busy.label");
    }
}
}  // namespace

int main(int, char** argv) {
    (void)argv;
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_tmp = L::FullPath(std::wstring(tmp) + L"melange-launcher-selftest");
    L::MakeDirs(g_tmp);
    g_bin = L::ExeDir();
    g_src = L::FullPath(L::Widen(MELANGE_SOURCE_DIR));
    TestVdf();
    TestDetect();
    TestExe();
    TestDll();
    if (L::FileExists(Ual())) {
        TestEngineFresh();
        TestEngineLoaders();
        TestEngineCleanup();
        TestEngineGuards();
        TestEngineRollback();
    }
    TestIniMerge();
    TestPluginSettings();
    TestRecommended();
    TestSettings();
    TestBusyStatusJson();
    TestStoreEngine();
    melange::store::Shutdown();
    Wipe(g_tmp);
    printf("launcher_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
