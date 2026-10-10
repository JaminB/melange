// Offline self-test for Melange.exe's setup logic: VDF parsing, game detection, exe validation, loader identity,
// the install/repair/uninstall/restore engine, Restore vanilla, Melange.ini merging, plugin settings, the recommended
// set and the updater (release parsing, file:/// downloads, staging, --apply-update). Works on fake game folders under
// %TEMP%; never touches a real game folder or the network.
// Exit code 0 = all passed.
#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/pe_laa.h"
#include "launcher/plugin_settings.h"
#include "launcher/recommended.h"
#include "launcher/settings.h"
#include "launcher/setup/detect.h"
#include "launcher/setup/dll_id.h"
#include "launcher/setup/engine.h"
#include "launcher/setup/exe_check.h"
#include "launcher/setup/ini_merge.h"
#include "launcher/setup/laa.h"
#include "launcher/setup/running.h"
#include "launcher/setup/vanilla.h"
#include "launcher/setup/vdf.h"
#include "launcher/updater.h"
#include "launcher/util.h"
#include "oasis/rpc/ini_edit.h"
#include "store/index.h"
#include "store/store.h"
#include "tools/hash.h"
#include "tools/json_read.h"
#include "update/release.h"

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
    return {S::Profile{probe.exe.size, probe.exe.timestamp, melange::pe::CanonicalSha256(exe), "Test #1"}};
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

// ---------------------------------------------------------------- large-address-aware (4 GB) mode
void TestLaa() {
    const std::wstring dir = Fresh(L"laa");
    const std::wstring exe = dir + L"\\WormsMayhem.exe", tmp = exe + L".melange-tmp", marker = dir + L"\\Melange\\laa.json";
    Copy(L::ExePath(), exe);
    melange::pe::SetLaaInFile(exe, false);   // the build's stock state, whatever the linker did to this test exe
    const std::string raw0 = melange::hashutil::Sha256HexFile(exe), canon0 = melange::pe::CanonicalSha256(exe);
    uint16_t chars0 = 0, chars1 = 0;
    uint32_t ts0 = 0, ts1 = 0, sum0 = 0, sum1 = 0;
    Expect(melange::pe::ReadPeFlags(exe, &chars0, &ts0, &sum0) && (chars0 & 0x20) == 0, "laa: stock copy has the bit clear");
    Expect(canon0 == raw0, "laa: canonical hash of a stock exe is its raw hash");

    // The hash ignores the bit and nothing else.
    Expect(melange::pe::SetLaaInFile(exe, true) == 0 && melange::pe::IsLaaFile(exe), "laa: SetLaaInFile sets the bit");
    Expect(melange::pe::ReadPeFlags(exe, &chars1, &ts1, &sum1) && chars1 == (chars0 | 0x20), "laa: only the 0x0020 bit changed", std::to_string(chars1));
    Expect(ts1 == ts0 && sum1 == sum0, "laa: timestamp and CheckSum untouched");
    Expect(melange::pe::CanonicalSha256(exe) == canon0, "laa: patched and unpatched give the same canonical hash");
    Expect(melange::hashutil::Sha256HexFile(exe) != raw0, "laa: the raw hash does differ");
    Expect(melange::pe::SetLaaInFile(exe, true) == 0 && melange::pe::SetLaaInFile(exe, false) == 0 && melange::hashutil::Sha256HexFile(exe) == raw0,
           "laa: clearing gives the original bytes back");
    const std::wstring junk = dir + L"\\junk.bin";
    Put(junk, "MZ this is not a portable executable");
    Expect(!melange::pe::ReadPeFlags(junk, &chars1) && melange::pe::CanonicalSha256(junk) == melange::hashutil::Sha256HexFile(junk) &&
               melange::pe::SetLaaInFile(junk, true) != 0,
           "laa: a file without a PE header is hashed raw and not patched");

    // The launcher sees a patched copy as the same build.
    auto profiles = FakeProfiles(exe);
    Expect(profiles[0].sha256 == canon0, "laa: the profile hash is the canonical one");
    melange::pe::SetLaaInFile(exe, true);
    S::ClearExeCache();
    S::GameCheck c = S::CheckExe(dir, profiles);
    Expect(c.verdict == S::Verdict::Ok && c.exe.laa && c.exe.sha256 == canon0, "laa: a patched exe is still verdict ok, laa reported");
    Expect(S::GameCheckJson(c).find("\"laa\":true") != std::string::npos, "laa: GameCheckJson carries laa", S::GameCheckJson(c));
    melange::pe::SetLaaInFile(exe, false);

    S::Context ctx;
    ctx.gameDir = dir;
    ctx.profiles = &profiles;
    ctx.version = "9.9.9";
    ctx.running = [](const std::wstring&) { return false; };

    // Apply: marker, checked swap, no temp file; again is a no-op; revert is byte for byte.
    S::LaaResult r = S::EnsureLaa(ctx, true);
    Expect(r.ok && r.changed && r.state == "applied" && melange::pe::IsLaaFile(exe), "laa: EnsureLaa(true) patches", r.message);
    Expect(L::FileExists(marker) && Get(marker).find("\"appliedBy\":\"melange\"") != std::string::npos && S::LaaMarkerPresent(dir),
           "laa: the marker records that Melange set it", Get(marker));
    Expect(!L::FileExists(tmp), "laa: no temp file left after applying");
    Expect(melange::pe::CanonicalSha256(exe) == canon0 && S::CheckExe(dir, profiles).verdict == S::Verdict::Ok, "laa: still the known build afterwards");
    const std::string patched = melange::hashutil::Sha256HexFile(exe);
    r = S::EnsureLaa(ctx, true);
    Expect(r.ok && !r.changed && r.state == "unchanged" && melange::hashutil::Sha256HexFile(exe) == patched && !L::FileExists(tmp),
           "laa: applying twice changes nothing");
    r = S::EnsureLaa(ctx, false);
    Expect(r.ok && r.changed && r.state == "reverted" && melange::hashutil::Sha256HexFile(exe) == raw0, "laa: revert restores the original bytes", r.message);
    Expect(!L::FileExists(marker) && !L::FileExists(tmp), "laa: revert removes the marker and leaves no temp file");
    r = S::EnsureLaa(ctx, false);
    Expect(r.ok && !r.changed && r.state == "unchanged", "laa: reverting a stock exe is a no-op");

    // An exe patched by something else is not Melange's to undo, unless asked outright.
    melange::pe::SetLaaInFile(exe, true);
    r = S::EnsureLaa(ctx, false);
    Expect(r.ok && !r.changed && r.state == "external" && melange::pe::IsLaaFile(exe), "laa: an external patch without the marker is not reverted");
    r = S::EnsureLaa(ctx, false, true);
    Expect(r.ok && r.changed && !melange::pe::IsLaaFile(exe) && melange::hashutil::Sha256HexFile(exe) == raw0, "laa: an explicit revert clears it", r.message);

    // Refusals leave the exe as it was.
    auto other = profiles;
    other[0].sha256 = std::string(64, '0');
    S::Context unknown = ctx;
    unknown.profiles = &other;
    S::ClearExeCache();
    r = S::EnsureLaa(unknown, true);
    Expect(!r.ok && r.state == "refused" && !melange::pe::IsLaaFile(exe) && !L::FileExists(marker), "laa: an unknown exe is never patched");
    S::Context busy = ctx;
    busy.running = [](const std::wstring&) { return true; };
    r = S::EnsureLaa(busy, true);
    Expect(!r.ok && r.state == "refused" && r.message == "Close Worms Ultimate Mayhem first." && !melange::pe::IsLaaFile(exe),
           "laa: refused while the game runs", r.message);
    S::ClearExeCache();
    HANDLE lock = CreateFileW(exe.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    r = S::EnsureLaa(ctx, true);
    CloseHandle(lock);
    Expect(!r.ok && !r.changed && !L::FileExists(tmp) && !L::FileExists(marker) && melange::hashutil::Sha256HexFile(exe) == raw0,
           "laa: an exclusively locked exe is left alone", r.message);
    // Held open for reading without delete sharing, as a mapped image is: the copy works, the swap does not.
    S::ClearExeCache();
    lock = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    r = S::EnsureLaa(ctx, true);
    CloseHandle(lock);
    Expect(!r.ok && !r.changed && r.state == "failed" && !L::FileExists(tmp) && !L::FileExists(marker) && melange::hashutil::Sha256HexFile(exe) == raw0,
           "laa: a failed swap removes the temp file and the marker and leaves the exe", r.message);
    r = S::EnsureLaa(ctx, true);
    Expect(r.ok && r.changed, "laa: and works once the lock is gone", r.message);
    Put(tmp, "stale");
    r = S::EnsureLaa(ctx, false);
    Expect(r.ok && !L::FileExists(tmp), "laa: a stale temp file is swept");

    // The setting: one key in Melange.ini, every other line kept.
    const std::string ini = "; mine\r\n[Display]\r\nFullscreen=1 ; yes\r\n\r\n[Game]\r\nOther=5\r\n";
    Put(dir + L"\\Melange.ini", ini);
    Expect(!S::IniWantsLaa(dir), "laa: ini default is off");
    Expect(S::SetIniLaa(dir, true).empty() && S::IniWantsLaa(dir), "laa: SetIniLaa(true)");
    std::string now = Get(dir + L"\\Melange.ini");
    Expect(now == "; mine\r\n[Display]\r\nFullscreen=1 ; yes\r\n\r\n[Game]\r\nOther=5\r\nLargeAddressAware=1\r\n", "laa: only the new key was added", now);
    Expect(S::SetIniLaa(dir, false).empty() && !S::IniWantsLaa(dir) && Get(dir + L"\\Melange.ini").find("LargeAddressAware=0") != std::string::npos &&
               Get(dir + L"\\Melange.ini").find("Fullscreen=1 ; yes") != std::string::npos,
           "laa: SetIniLaa(false) keeps the other keys");
    Expect(!S::SetIniLaa(dir + L"\\nope", true).empty(), "laa: no ini, no write");
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

// ---------------------------------------------------------------- restore vanilla
bool HasPath(const std::vector<std::string>& v, const char* path) {
    for (const auto& x : v)
        if (L::IEquals(x, path)) return true;
    return false;
}
const S::VanillaGroup* Group(const S::VanillaPlan& p, const char* id) {
    for (const auto& g : p.groups)
        if (g.id == id) return &g;
    return nullptr;
}
std::string GroupsText(const S::VanillaPlan& p) {
    std::string t;
    for (const auto& g : p.groups) t += g.id + "=" + g.label + " x" + std::to_string(g.files) + "; ";
    return t + (p.refused.empty() ? "" : " refused: " + p.refused);
}

void TestStockList() {
    S::StockList l;
    std::string err;
    const std::string tsv = Get(g_src + L"\\res\\wum-1077-stock.tsv");
    Expect(S::ParseStockList(tsv, &l, &err), "stock: the shipped list parses", err);
    Expect(l.files.size() == 2130, "stock: 2130 files", std::to_string(l.files.size()));
    const auto exe = l.files.find(L"wormsmayhem.exe");
    Expect(exe != l.files.end() && exe->second == 5713408, "stock: WormsMayhem.exe #1077 size");
    Expect(l.dirs.count(L"data") && l.dirs.count(L"cg") && !l.dirs.count(L"redist"), "stock: folders");
    Expect(S::EmbeddedStockList().files.size() == 2130, "stock: embedded as WUM_STOCK", std::to_string(S::EmbeddedStockList().files.size()));
    Expect(!S::ParseStockList("..\\x.dll\t1\r\n", &l, &err), "stock: traversal refused");
    Expect(!S::ParseStockList("a.dll\tx\r\n", &l, &err), "stock: bad size refused");
    Expect(!S::ParseStockList("# only a comment\n", &l, &err), "stock: empty refused");
}

// A folder linked into the game folder (a mod author's junctioned Mods\) goes as a link: what it points at stays.
void TestVanillaLink() {
    Rig r = MakeRig(L"vanilla-link");
    const std::wstring g = r.game, outside = L::Parent(g) + L"\\elsewhere";
    Put(outside + L"\\precious\\work.lua", "keep me");
    Put(outside + L"\\top.txt", "keep me too");
    const std::wstring cmd = L"cmd /c mklink /J \"" + g + L"\\Mods\" \"" + outside + L"\" >nul";
    const bool linked = _wsystem(cmd.c_str()) == 0 && L::FileExists(g + L"\\Mods\\top.txt");
    Expect(linked, "vanilla link: junction made");
    if (!linked) return;
    S::StockList stock;
    std::string err;
    Expect(S::ParseStockList("WormsMayhem.exe\t" + std::to_string(L::FileSize(g + L"\\WormsMayhem.exe")) + "\r\n", &stock, &err),
           "vanilla link: list parses", err);
    S::VanillaContext v;
    v.base = r.ctx;
    v.base.selfExe = r.payload + L"\\Melange.exe";
    v.stock = &stock;
    v.replaysDir = L::Parent(g) + L"\\Documents\\Melange\\replays";
    v.base.storeOf = [](const std::wstring&) { return std::string("steam"); };
    const S::VanillaPlan p = S::MakeVanillaPlan(v);
    Expect(p.refused.empty(), "vanilla link: plan runs", p.refused);
    Expect(!HasPath(p.remove, "Mods\\top.txt") && !HasPath(p.remove, "Mods\\precious\\work.lua"), "vanilla link: never planned through the link");
    const S::VanillaOutcome o = S::ApplyVanilla(v, p.planId);
    Expect(o.outcome.ok, "vanilla link: applied", o.outcome.message);
    Expect(!L::DirExists(g + L"\\Mods"), "vanilla link: the link itself is gone");
    Expect(Get(outside + L"\\top.txt") == "keep me too" && Get(outside + L"\\precious\\work.lua") == "keep me",
           "vanilla link: everything it pointed at is untouched");
}

void TestVanilla() {
    Rig r = MakeRig(L"vanilla");
    const std::wstring g = r.game, docs = L::Parent(g) + L"\\Documents\\Melange\\replays";
    // The stock game: what the injected list names.
    Put(g + L"\\CG\\FixedFunction.cg", "stock cg");
    Put(g + L"\\Data\\Frontend\\menu.xom", "modified by a mod");   // stock size 5: overwritten
    Put(g + L"\\Data\\Maps\\Stock.xom", "map");
    Put(g + L"\\Default.cfg", "defaults");
    const std::string tsv = "WormsMayhem.exe\t" + std::to_string(L::FileSize(g + L"\\WormsMayhem.exe")) +
                            "\r\nCG\\FixedFunction.cg\t8\r\nData\\level.xom\t9\r\nData\\Frontend\\menu.xom\t5\r\nData\\Maps\\Stock.xom\t0\r\n"
                            "Data\\gone.xom\t3\r\nDefault.cfg\t8\r\n";
    S::StockList stock;
    std::string err;
    Expect(S::ParseStockList(tsv, &stock, &err), "vanilla: test list parses", err);
    // Files the game writes: kept.
    Put(g + L"\\local.cfg", "mine");
    Put(g + L"\\steam_appid.txt", "70600");
    Put(g + L"\\Data\\Shaders\\water.csh", "cache");
    Put(g + L"\\XOM2-1.log", "engine");
    Put(g + L"\\Net_1.log", "net");
    Put(g + L"\\Redist\\vcredist_x86.exe", "redist");
    // Melange (7 files, the replays aside; an engine log copied into its folder goes too).
    Copy(g_bin + L"\\fake_asi_0_4_0.dll", g + L"\\melange.asi");
    Put(g + L"\\Melange.ini", kTemplate);
    Copy(g_bin + L"\\fake_dinput8_other.dll", g + L"\\Melange.exe");
    Put(g + L"\\Melange\\install.json", "{}");
    Put(g + L"\\Melange\\backup\\x\\manifest.json", "{}");
    Put(g + L"\\Mods\\hello\\spice.json", "{}");
    Put(g + L"\\Melange\\replays\\match.wsr", "replay 1");
    Put(g + L"\\Melange\\logs\\desync-1.zip", "bundle");
    Put(g + L"\\Melange\\logs\\XOM1-2.log", "copy");
    Copy(Ual(), g + L"\\dinput8.dll");
    // ReShade as opengl32.dll.
    Copy(g_bin + L"\\fake_dinput8_reshade.dll", g + L"\\opengl32.dll");
    Put(g + L"\\ReShade.ini", "[GENERAL]");
    Put(g + L"\\reshade-shaders\\Shaders\\x.fx", "fx");
    // Renewation HD.
    Put(g + L"\\Version.txt", "Renewation HD 0.2A2\r\nby the team\r\n");
    Put(g + L"\\plugins\\patch.asi", "patch");
    Put(g + L"\\plugins\\patch.ini", "[Patch]");
    Put(g + L"\\Credits.txt", "credits");
    Put(g + L"\\Data\\AlexBond_x.XOM", "map");
    // WUMPatch.
    Put(g + L"\\plugins\\WUM.Patch.asi", "patch");
    Put(g + L"\\Data2\\x.xom", "x");
    Put(g + L"\\Data\\Language\\PC\\Chinese.xom", "zh");
    // Loose plugins and leftovers.
    Put(g + L"\\scripts\\foo.asi", "asi");
    Put(g + L"\\bar.asi", "asi");
    Put(g + L"\\notes.txt", "notes");
    // A replay already in Documents with the same name: never overwritten.
    Put(docs + L"\\match.wsr", "older replay");

    S::VanillaContext v;
    v.base = r.ctx;
    v.base.selfExe = r.payload + L"\\Melange.exe";
    v.stock = &stock;
    v.replaysDir = docs;
    v.base.storeOf = [](const std::wstring&) { return std::string("steam"); };

    // Refused while the game runs; nothing changes.
    const auto before = Snap(g);
    S::VanillaContext running = v;
    running.base.running = [](const std::wstring&) { return true; };
    S::VanillaPlan p = S::MakeVanillaPlan(running);
    Expect(p.refused == "Close Worms Ultimate Mayhem first.", "vanilla: refused while the game runs", p.refused);
    Expect(!S::ApplyVanilla(running, "").outcome.ok && Snap(g) == before, "vanilla: apply refused while running, nothing changed");
    // Refused on another build.
    auto bad = *r.profiles;
    bad[0].sha256 = std::string(64, 'f');
    S::ClearExeCache();
    S::VanillaContext wrong = v;
    wrong.base.profiles = &bad;
    Expect(S::MakeVanillaPlan(wrong).refused.find("build #1077") != std::string::npos, "vanilla: refused on another build");
    S::ClearExeCache();

    p = S::MakeVanillaPlan(v);
    Expect(p.refused.empty(), "vanilla: plan runs", p.refused);
    Expect(Group(p, "melange") && Group(p, "melange")->files == 7, "vanilla: Melange found", GroupsText(p));
    Expect(Group(p, "renewation") && Group(p, "renewation")->label == "Renewation HD 0.2A2" && Group(p, "renewation")->files == 5,
           "vanilla: Renewation named with its version", GroupsText(p));
    Expect(Group(p, "wumpatch") && Group(p, "wumpatch")->files == 3, "vanilla: WUMPatch found", GroupsText(p));
    Expect(Group(p, "loader") && Group(p, "loader")->label == "Ultimate ASI Loader 9.7.4 (dinput8.dll)", "vanilla: UAL named", GroupsText(p));
    Expect(Group(p, "reshade") && Group(p, "reshade")->label == "ReShade 6.3.0.0" && Group(p, "reshade")->files == 3,
           "vanilla: ReShade as opengl32.dll, with its files", GroupsText(p));
    Expect(Group(p, "asi") && Group(p, "asi")->files == 2, "vanilla: two loose ASI plugins", GroupsText(p));
    Expect(Group(p, "other") && Group(p, "other")->files == 1 && p.groups.back().id == "other", "vanilla: one other file, listed last", GroupsText(p));
    Expect(p.remove.size() == 22, "vanilla: 22 files to delete", std::to_string(p.remove.size()));
    for (const char* kept : {"local.cfg", "steam_appid.txt", "Data\\Shaders\\water.csh", "XOM2-1.log", "Net_1.log", "Redist\\vcredist_x86.exe",
                             "Default.cfg", "WormsMayhem.exe", "Data\\Frontend\\menu.xom"})
        Expect(!HasPath(p.remove, kept), "vanilla: not deleted", kept);
    Expect(p.replays.size() == 2 && HasPath(p.replays, "Melange\\replays\\match.wsr") && HasPath(p.replays, "Melange\\logs\\desync-1.zip"),
           "vanilla: replays and desync bundles are moved");
    Expect(p.modified.size() == 1 && L::IEquals(p.modified[0], "Data\\Frontend\\menu.xom"), "vanilla: the overwritten stock file");
    Expect(p.missing.size() == 1 && L::IEquals(p.missing[0], "data\\gone.xom"), "vanilla: the missing stock file");
    Expect(p.verify && p.overwrites && p.store == "steam" && !p.selfInGame, "vanilla: verify needed");

    Expect(S::ApplyVanilla(v, "stale").outcome.code == -32013, "vanilla: plan id mismatch");
    // Access denied on the first delete: nothing deleted.
    S::VanillaContext denied = v;
    denied.remove = [](const std::wstring&) -> unsigned long { return ERROR_ACCESS_DENIED; };
    S::VanillaOutcome o = S::ApplyVanilla(denied, p.planId);
    Expect(!o.outcome.ok && o.outcome.code == -32010 && o.deleted == 0, "vanilla: access denied is -32010", o.outcome.message);
    Expect(L::FileExists(g + L"\\melange.asi") && L::FileExists(g + L"\\notes.txt"), "vanilla: access denied deleted nothing");
    Expect(Get(docs + L"\\match.wsr") == "older replay", "vanilla: an existing replay is never overwritten");
    // The replays went out first and stay safe; put them back so the real run sees the same plan.
    for (const auto& [from, to] : o.moved) MoveFileW(L::Widen(to).c_str(), (g + L"\\" + L::Widen(from)).c_str());

    // Melange.exe run from the game folder is left for after exit.
    v.base.selfExe = g + L"\\Melange.exe";
    p = S::MakeVanillaPlan(v);
    Expect(p.selfInGame, "vanilla: the running exe is in the game folder");
    o = S::ApplyVanilla(v, p.planId);
    Expect(o.outcome.ok, "vanilla: applied", o.outcome.message);
    Expect(o.deleted == 21 && o.pending.size() == 1 && L::FileExists(g + L"\\Melange.exe"), "vanilla: 21 deleted, Melange.exe pending",
           std::to_string(o.deleted));
    DeleteFileW((g + L"\\Melange.exe").c_str());
    const auto after = Snap(g);
    std::string left;
    for (const auto& [k, sha] : after) left += L::Narrow(k) + " ";
    Expect(after.size() == 12, "vanilla: only stock and kept files remain", left);
    for (const wchar_t* gone : {L"Melange", L"Mods", L"Data2", L"plugins", L"scripts", L"reshade-shaders"})
        Expect(!L::DirExists(g + L"\\" + gone), "vanilla: folder removed", L::Narrow(gone));
    Expect(L::DirExists(g + L"\\Data\\Shaders") && L::DirExists(g + L"\\Redist") && L::DirExists(g + L"\\Data\\Maps"), "vanilla: game folders kept");
    Expect(Get(g + L"\\Data\\Frontend\\menu.xom") == "modified by a mod" && Get(g + L"\\local.cfg") == "mine", "vanilla: stock and user files untouched");
    Expect(Get(docs + L"\\match.wsr") == "older replay" && Get(docs + L"\\match (2).wsr") == "replay 1" && Get(docs + L"\\desync-1.zip") == "bundle",
           "vanilla: replays moved to Documents, suffixed instead of overwritten");
    Expect(o.moved.size() == 2, "vanilla: two replays moved");
    // Again: nothing left but the verify.
    p = S::MakeVanillaPlan(v);
    Expect(p.remove.empty() && p.replays.empty() && p.groups.empty() && !p.overwrites && p.verify, "vanilla: second plan is empty", GroupsText(p));

    // launcher.json: forget the game, keep the theme and window.
    const std::wstring dir = Fresh(L"vanilla-settings");
    L::Settings s;
    s.gameDir = g;
    s.firstRunDone = true;
    s.theme = "dark";
    s.window.saved = true;
    s.window.right = 900;
    s.defaults.push_back(L::DefaultPlugin{"sunstone", true, {}});
    s.defaultsSeeded = true;
    s.lastUpdateCheck = "2026-10-05T10:00:00Z";
    L::ResetForVanilla(&s);
    Expect(L::SaveSettings(dir + L"\\launcher.json", s), "vanilla: launcher.json saved");
    L::Settings t;
    Expect(L::LoadSettings(dir + L"\\launcher.json", &t) && t.gameDir.empty() && !t.firstRunDone && t.defaults.empty() && !t.defaultsSeeded &&
               t.lastUpdateCheck.empty() && t.theme == "dark" && t.window.saved && t.window.right == 900,
           "vanilla: launcher reset to first run, theme and window kept");
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

std::string PluginSpice(const std::string& id, const std::string& version, const std::string& range) {
    return "{\"spiceVersion\":1,\"id\":\"" + id + "\",\"version\":\"" + version + "\",\"name\":\"Hello\",\"authors\":[\"me\"],\"melange\":{\"range\":\"" +
           range + "\"},\"kind\":\"client-only\",\"entry\":{\"client\":\"client/init.lua\"},\"settings\":[{\"key\":\"level\",\"type\":\"int\","
           "\"default\":2,\"min\":1,\"max\":5,\"label\":\"Level\"}]}";
}

std::string MakePluginZip(const std::string& version, uint64_t* unpacked, const std::string& id = "hello") {
    const std::string spice = PluginSpice(id, version, ">=0.1.0");
    const std::pair<std::string, std::string> files[] = {
        {id + "/spice.json", spice}, {id + "/LICENSE", "MIT"}, {id + "/client/init.lua", "wum.log.info('hello')"}};
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

// The compatibility sweep against a Mods folder: a local plugin moves to Mods\.incompatible, a Store one with a
// compatible newer version is updated to it, a Store one with none is removed; nothing changes without a list.
void TestStoreReconcile() {
    namespace st = melange::store;
    namespace cp = melange::compat;
    const std::wstring root = Fresh(L"reconcile");
    const std::wstring mods = root + L"\\game\\Mods";
    uint64_t unpacked = 0;
    const std::string zip = MakePluginZip("1.1.0", &unpacked);
    Put(root + L"\\index\\hello-1.1.0.zip", zip);
    const std::string sha = melange::hashutil::Sha256Hex(zip.data(), zip.size());
    auto ver = [&](const std::string& v, const std::string& range, const std::string& url, const std::string& hash, uint64_t size) {
        return "{\"version\":\"" + v + "\",\"released\":\"2026-10-03\",\"melange\":\"" + range + "\",\"kind\":\"client-only\","
               "\"permissions\":{\"unsafe\":false,\"filesystem\":\"none\"},\"url\":\"" + url + "\",\"sha256\":\"" + hash +
               "\",\"size\":" + std::to_string(size) + ",\"unpackedSize\":" + std::to_string(unpacked) + ",\"files\":3}";
    };
    auto plugin = [](const std::string& id, const std::string& versions) {
        return "{\"id\":\"" + id + "\",\"name\":\"" + id + " plugin\",\"authors\":[\"me\"],\"description\":\"d\",\"licence\":\"MIT\","
               "\"categories\":[\"misc\"],\"gameBuilds\":[\"1077\"],\"versions\":[" + versions + "]}";
    };
    Put(root + L"\\index\\index.json",
        "{\"indexVersion\":1,\"serial\":1,\"plugins\":[" +
            plugin("hello", ver("1.1.0", ">=0.1.0", "hello-1.1.0.zip", sha, zip.size()) + "," +
                                ver("1.0.0", ">=9.0.0", "hello-1.0.0.zip", std::string(64, 'a'), 100)) + "," +
            plugin("gone", ver("1.0.0", ">=9.0.0", "gone-1.0.0.zip", std::string(64, 'a'), 100)) + "]}");
    // Installed from the Store (installed.json), but asking for a Melange this is not.
    Put(mods + L"\\hello\\spice.json", PluginSpice("hello", "1.0.0", ">=9.0.0"));
    Put(mods + L"\\hello\\user\\keep.txt", "mine");
    Put(mods + L"\\gone\\spice.json", PluginSpice("gone", "1.0.0", ">=9.0.0"));
    Put(mods + L"\\mine\\spice.json", PluginSpice("mine", "1.0.0", ">=9.0.0"));
    Put(mods + L"\\fine\\spice.json", PluginSpice("fine", "1.0.0", ">=0.1.0"));
    Put(mods + L"\\.store\\installed.json",
        "{\"_serialSeen\":0,\"hello\":{\"version\":\"1.0.0\",\"sha256\":\"\",\"installedAt\":\"\",\"serial\":1},"
        "\"gone\":{\"version\":\"1.0.0\",\"sha256\":\"\",\"installedAt\":\"\",\"serial\":1}}");
    std::string url = "file:///" + L::Narrow(root + L"\\index\\index.json");
    for (char& c : url)
        if (c == '\\') c = '/';
    FakeStoreHost host;
    host.mods = mods;
    st::Config cfg;
    cfg.indexUrl = url;
    cfg.custom = true;
    st::SetHost(&host, cfg);
    Expect(st::Open(mods), "reconcile: store opened");

    cp::SweepContext c;
    c.modsDir = mods;
    c.melangeVersion = "0.4.0";
    std::vector<std::string> forgot;
    c.forget = [&](const std::string& id) { forgot.push_back(id); };
    const cp::Report r = cp::Sweep(c);
    Expect(r.quarantined.size() == 1 && r.quarantined[0].id == "mine" && L::DirExists(mods + L"\\.incompatible\\mine") &&
               !L::DirExists(mods + L"\\mine") && forgot == std::vector<std::string>{"mine"},
           "reconcile: the local plugin was quarantined");
    Expect(r.store.size() == 2 && L::DirExists(mods + L"\\hello") && L::DirExists(mods + L"\\gone"), "reconcile: Store plugins are left to the Store");

    // No list in memory and none cached: nothing changes (the game without a fetch).
    Expect(st::Reconcile(r.store, false), "reconcile: queued without a fetch");
    Expect(WaitFor([] { return !st::GetStatus().busy; }), "reconcile: finished without a fetch");
    Expect(Get(mods + L"\\hello\\spice.json").find("\"1.0.0\"") != std::string::npos && L::DirExists(mods + L"\\gone"),
           "reconcile: without a list nothing is updated or removed");

    Expect(st::Reconcile(r.store, true), "reconcile: queued with a fetch");
    Expect(WaitFor([] { return !st::GetStatus().busy; }), "reconcile: finished with a fetch");
    Expect(Get(mods + L"\\hello\\spice.json").find("\"1.1.0\"") != std::string::npos, "reconcile: hello updated to the version that loads",
           st::GetStatus().job.message);
    Expect(Get(mods + L"\\hello\\user\\keep.txt") == "mine", "reconcile: the update kept hello's user folder");
    Expect(!L::DirExists(mods + L"\\gone"), "reconcile: gone (no version loads) was removed");
    Expect(L::DirExists(mods + L"\\fine"), "reconcile: a plugin that loads is untouched");

    // With the list known but not fetched just now (the game, or offline), a plugin no version of which loads stays:
    // the release that fixes it may be newer than that list.
    Put(mods + L"\\later\\spice.json", PluginSpice("later", "1.0.0", ">=9.0.0"));
    {
        std::string dbText = Get(mods + L"\\.store\\installed.json");
        dbText.insert(dbText.rfind('}'), ",\"later\":{\"version\":\"1.0.0\",\"sha256\":\"\",\"installedAt\":\"\",\"serial\":1}");
        Put(mods + L"\\.store\\installed.json", dbText);
    }
    st::Close();
    Expect(st::Open(mods), "reconcile: store reopened");
    Expect(st::Reconcile({{"later", "later", "1.0.0", "needs Melange >=9.0.0, you have 0.4.0"}}, false), "reconcile: queued from the cached list");
    Expect(WaitFor([] { return !st::GetStatus().busy; }), "reconcile: finished from the cached list");
    Expect(L::DirExists(mods + L"\\later"), "reconcile: without a fresh list a plugin is not removed");
    const std::string db = Get(mods + L"\\.store\\installed.json");
    Expect(db.find("\"gone\"") == std::string::npos && db.find("\"1.1.0\"") != std::string::npos, "reconcile: installed.json follows", db);
    bool forgotGone = false;
    for (const auto& call : host.calls) forgotGone |= call == "forget gone";
    Expect(forgotGone, "reconcile: the host forgot the removed plugin");
    std::string actions;
    for (const cp::Notice& n : cp::LoadNotices(mods)) actions += n.id + ":" + n.action + (n.detail.empty() ? "" : "=" + n.detail) + " ";
    Expect(actions == "mine:quarantined gone:removed hello:updated=1.1.0 ", "reconcile: one notice per action", actions);
    st::Close();
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
    s.lastUpdateCheck = "2026-10-05T10:00:00Z";
    Expect(L::SaveSettings(dir + L"\\launcher.json", s), "launcher.json: saved");
    L::Settings t;
    Expect(L::LoadSettings(dir + L"\\launcher.json", &t), "launcher.json: loaded");
    Expect(t.gameDir == s.gameDir && t.firstRunDone && t.theme == "dark" && t.defaultsSeeded && t.defaults.size() == 1 &&
               t.defaults[0].settings["quality"].str == "bold" && t.defaults[0].settings["water"].b && t.lastUpdateCheck == s.lastUpdateCheck,
           "launcher.json: round trip");
    Put(dir + L"\\bad.json", "{not json");
    Expect(!L::LoadSettings(dir + L"\\bad.json", &t) && t.gameDir.empty() && t.theme == "system", "launcher.json: bad file -> defaults");
    Expect(t.autoUpdate, "launcher.json: automatic update checks default on");
    Put(dir + L"\\old.json", "{\"version\":1,\"theme\":\"dark\"}");
    Expect(L::LoadSettings(dir + L"\\old.json", &t) && t.autoUpdate, "launcher.json: autoUpdate missing -> on");
    s.autoUpdate = false;
    Expect(L::SaveSettings(dir + L"\\launcher.json", s) && L::LoadSettings(dir + L"\\launcher.json", &t) && !t.autoUpdate,
           "launcher.json: autoUpdate off round trip");
}

// Settings › Updates writes the game's [Update] CheckInGame: only when it disagrees, keeping every other byte and
// the file's encoding.
void TestInGameCheckSync() {
    namespace U = L::updater;
    const std::wstring game = Fresh(L"ingame-check");
    bool changed = true;
    std::string err, bytes;
    Expect(U::SyncInGameCheck(game, false, &changed, &err) && !changed && !L::FileExists(game + L"\\Melange.ini"),
           "CheckInGame: no Melange.ini -> nothing written", err);
    Expect(U::SyncInGameCheck(L"", false, &changed, &err) && !changed, "CheckInGame: no game folder -> nothing", err);
    const std::string ini = "; Melange\r\n[Update]\r\nEnabled=1\r\nCheckInGame=1            ; once a day\r\n\r\n[Other]\r\nX=2\r\n";
    Put(game + L"\\Melange.ini", ini);
    Expect(U::SyncInGameCheck(game, true, &changed, &err) && !changed, "CheckInGame: already on -> untouched", err);
    Expect(U::SyncInGameCheck(game, false, &changed, &err) && changed, "CheckInGame: turned off", err);
    L::ReadAll(game + L"\\Melange.ini", &bytes);
    Expect(bytes == "; Melange\r\n[Update]\r\nEnabled=1\r\nCheckInGame=0            ; once a day\r\n\r\n[Other]\r\nX=2\r\n",
           "CheckInGame: only the value changed", bytes);
    Expect(U::SyncInGameCheck(game, false, &changed, &err) && !changed, "CheckInGame: already off -> untouched", err);
    Expect(U::SyncInGameCheck(game, true, &changed, &err) && changed, "CheckInGame: turned back on", err);
    L::ReadAll(game + L"\\Melange.ini", &bytes);
    Expect(bytes == ini, "CheckInGame: back to the original bytes", bytes);
    // Missing key reads as on (melange.asi's default): off adds it, on leaves the file alone.
    Put(game + L"\\Melange.ini", "[Update]\r\nEnabled=1\r\n");
    Expect(U::SyncInGameCheck(game, true, &changed, &err) && !changed, "CheckInGame: missing key is on", err);
    Expect(U::SyncInGameCheck(game, false, &changed, &err) && changed, "CheckInGame: missing key -> written", err);
    L::ReadAll(game + L"\\Melange.ini", &bytes);
    Expect(bytes == "[Update]\r\nEnabled=1\r\nCheckInGame=0\r\n", "CheckInGame: added after the section's last key", bytes);
    // A UTF-16 file stays UTF-16.
    std::string wide;
    melange::oasis::ini::Encode("[Update]\r\nCheckInGame=1\r\n", melange::oasis::ini::Encoding::Utf16Le, &wide);
    Put(game + L"\\Melange.ini", wide);
    Expect(U::SyncInGameCheck(game, false, &changed, &err) && changed, "CheckInGame: UTF-16 turned off", err);
    L::ReadAll(game + L"\\Melange.ini", &bytes);
    melange::oasis::ini::Encoding enc{};
    const std::string text = melange::oasis::ini::Decode(bytes, &enc);
    Expect(enc == melange::oasis::ini::Encoding::Utf16Le && text == "[Update]\r\nCheckInGame=0\r\n", "CheckInGame: UTF-16 kept", text);
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

// ---------------------------------------------------------------- Melange updating itself
namespace U = melange::launcher::updater;
namespace R = melange::update;

std::string FileUrl(const std::wstring& path) {
    std::string s = "file:///" + L::Narrow(path);
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

std::string Zip(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 0);
    for (const auto& [name, data] : files) mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);
    void* buf = nullptr;
    size_t n = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &n);
    std::string out(static_cast<const char*>(buf), n);
    mz_zip_writer_end(&z);
    return out;
}

void TestUpdateRelease() {
    Expect(R::CompareVersions("0.3.7", "0.3.6") > 0 && R::CompareVersions("0.3.6", "0.3.6.0") == 0 && R::CompareVersions("0.10.0", "0.9.9") > 0 &&
               R::CompareVersions("1.0.0-beta", "1.0.0") == 0,
           "update: version compare");
    Expect(R::PlainVersion("0.3.7") && R::PlainVersion("1") && !R::PlainVersion("") && !R::PlainVersion("1..2") && !R::PlainVersion("1.2.") &&
               !R::PlainVersion("v1.2") && !R::PlainVersion("1.2.3-rc1") && !R::PlainVersion("1.2.3.4.5") && !R::PlainVersion("..\\x"),
           "update: plain versions only");

    std::string text = Get(g_src + L"\\tests\\fixtures\\launcher\\update\\github-latest.json");
    R::Release rel;
    std::string err;
    Expect(R::ParseRelease(text, &rel, &err), "update: GitHub's release object parses", err);
    Expect(rel.version == "0.3.7" && rel.tag == "v0.3.7" && rel.htmlUrl == "https://github.com/JaminB/melange/releases/tag/v0.3.7",
           "update: version and page from the release", rel.version);
    const R::Asset* m = rel.Find(rel.ManifestName());
    const R::Asset* z = rel.Find(rel.ZipName());
    Expect(m && z && z->size == 4718592 && m->url == "https://github.com/JaminB/melange/releases/download/v0.3.7/melange-0.3.7.json",
           "update: the manifest and zip assets");
    Expect(z && R::AllowedUrl(z->url, R::kDownloadPrefix), "update: release downloads are allowed");
    Expect(!R::AllowedUrl("https://github.com/JaminB/melange-fork/releases/download/v1/x.zip", R::kDownloadPrefix) &&
               !R::AllowedUrl("https://github.com/JaminB/melange/releases/download/../../evil/x.zip", R::kDownloadPrefix) &&
               !R::AllowedUrl("https://github.com/JaminB/melange/releases/download/v1/x.zip?redirect=evil", R::kDownloadPrefix) &&
               !R::AllowedUrl("http://github.com/JaminB/melange/releases/download/v1/x.zip", R::kDownloadPrefix) &&
               !R::AllowedUrl(R::kDownloadPrefix, R::kDownloadPrefix),
           "update: anything else is refused");
    for (const char* bad : {R"({"tag_name":"v1.0.0","draft":true})", R"({"tag_name":"v1.0.0","prerelease":true})", R"({"tag_name":"1.0.0"})",
                            R"({"tag_name":"v1.0.0-rc1"})", R"({"tag_name":"v../x"})", R"([])", "not json"})
        Expect(!R::ParseRelease(bad, &rel, &err), "update: refused release", bad);
    Expect(R::ParseRelease(R"({"tag_name":"v2.0.0","html_url":"javascript:x","assets":[{"name":"a"},5,{"name":"b","browser_download_url":"u","size":-1}]})",
                           &rel, &err) &&
               rel.htmlUrl.empty() && rel.assets.size() == 1 && rel.assets[0].size == 0,
           "update: odd assets and a non-https page are dropped");

    R::Manifest mf;
    text = Get(g_src + L"\\tests\\fixtures\\launcher\\update\\melange-0.3.7.json");
    Expect(R::ParseManifest(text, "0.3.7", &mf, &err) && mf.size == 4718592 && mf.zip == "melange-0.3.7.zip" &&
               mf.sha256 == "5d41402abc4b2a76b9719d911017c592ae2d4a7f3c6e1f0b8a9c7d6e5f4a3b2c",
           "update: manifest parses (sha256 lower-cased)", err);
    Expect(!R::ParseManifest(text, "0.3.8", &mf, &err) && err.find("expected 0.3.8") != std::string::npos, "update: manifest for another version", err);
    const std::string sha(64, 'a');
    const std::string head = "{\"version\":\"0.3.7\",\"zip\":\"melange-0.3.7.zip\",\"sha256\":\"";
    for (const std::string& bad : {"{\"version\":\"0.3.7\",\"zip\":\"other.zip\",\"sha256\":\"" + sha + "\",\"size\":1}",
                                   head + "abc\",\"size\":1}", head + sha + "\",\"size\":0}", head + sha + "\",\"size\":1.5}",
                                   head + sha + "\",\"size\":999999999999}", head + sha + "\"}", std::string("[1]")})
        Expect(!R::ParseManifest(bad, "0.3.7", &mf, &err), "update: refused manifest", bad);
}

void TestUpdateSigner() {
    U::Signer unsignedRun, a, b, bad;
    a.present = a.valid = true;
    a.subject = "CN=Jamin Becker, O=Jamin Becker, C=US";
    a.issuerOrg = "Microsoft Corporation";
    b = a;
    b.thumbprint = "renewed";
    bad.present = true;
    bad.error = "the file was changed after signing";
    U::Signer other = a;
    other.subject = "CN=Someone Else";
    U::Signer otherCa = a;
    otherCa.issuerOrg = "Another CA";
    Expect(U::SignerRefusal(unsignedRun, bad, "x").empty() && U::SignerRefusal(unsignedRun, U::Signer{}, "x").empty(),
           "signer: a developer build accepts anything");
    Expect(U::SignerRefusal(a, b, "x").empty(), "signer: same publisher across a certificate renewal");
    Expect(!U::SignerRefusal(a, U::Signer{}, "x").empty() && !U::SignerRefusal(a, bad, "x").empty(), "signer: unsigned or broken is refused");
    Expect(U::SignerRefusal(a, other, "Melange.exe").find("Someone Else") != std::string::npos, "signer: another publisher is refused");
    Expect(!U::SignerRefusal(a, otherCa, "x").empty(), "signer: another CA is refused");
    const U::Signer fake = U::ReadSigner(g_bin + L"\\fake_asi_9_0_0.dll");
    Expect(!fake.present && !fake.valid && fake.subject.empty(), "signer: an unsigned file reads as unsigned", fake.error);
    // Any Authenticode-signed binary on this PC, when there is one (Edge's is embedded-signed, not by catalog).
    wchar_t pf[MAX_PATH];
    for (const wchar_t* var : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        const DWORD n = GetEnvironmentVariableW(var, pf, MAX_PATH);
        const std::wstring edge = std::wstring(pf, n && n < MAX_PATH ? n : 0) + L"\\Microsoft\\Edge\\Application\\msedge.exe";
        if (!n || !L::FileExists(edge)) continue;
        const U::Signer s = U::ReadSigner(edge);
        Expect(s.present && s.valid && s.subject.find("Microsoft") != std::string::npos && !s.issuerOrg.empty() && s.thumbprint.size() == 40,
               "signer: a signed file reads its publisher", s.subject + " / " + s.issuerOrg + " / " + s.error);
        Expect(U::SignerRefusal(s, s, "x").empty() && !U::SignerRefusal(s, fake, "x").empty(), "signer: a signed launcher refuses an unsigned update");
        break;
    }
}

// A release served from file:/// fixtures: the latest answer, its manifest and the zip.
struct FakeRelease {
    std::wstring dir;
    std::string zip, latestUrl, prefix;
};
FakeRelease MakeRelease(const std::wstring& root, const std::string& version, const std::string& zipBytes, const std::string& manifestSha = "",
                        long long manifestSize = -1, long long assetSize = -1) {
    FakeRelease f;
    f.dir = root + L"\\server";
    f.zip = zipBytes;
    f.prefix = FileUrl(f.dir) + "/";
    const std::string zipName = "melange-" + version + ".zip", jsonName = "melange-" + version + ".json";
    Put(f.dir + L"\\" + L::Widen(zipName), zipBytes);
    const std::string sha = manifestSha.empty() ? melange::hashutil::Sha256Hex(zipBytes.data(), zipBytes.size()) : manifestSha;
    const long long size = manifestSize >= 0 ? manifestSize : static_cast<long long>(zipBytes.size());
    const long long as = assetSize >= 0 ? assetSize : static_cast<long long>(zipBytes.size());
    Put(f.dir + L"\\" + L::Widen(jsonName),
        "{\"version\":\"" + version + "\",\"zip\":\"" + zipName + "\",\"sha256\":\"" + sha + "\",\"size\":" + std::to_string(size) + "}");
    Put(f.dir + L"\\latest.json",
        "{\"tag_name\":\"v" + version + "\",\"html_url\":\"https://github.com/JaminB/melange/releases/tag/v" + version + "\",\"assets\":[" +
            "{\"name\":\"" + zipName + "\",\"size\":" + std::to_string(as) + ",\"browser_download_url\":\"" + f.prefix + zipName + "\"}," +
            "{\"name\":\"" + jsonName + "\",\"size\":100,\"browser_download_url\":\"" + f.prefix + jsonName + "\"}]}");
    f.latestUrl = f.prefix + "latest.json";
    return f;
}

std::string ReleaseZip(const wchar_t* binary = L"fake_asi_9_0_0.dll") {
    const std::string bin = Get(g_bin + L"\\" + binary);
    return Zip({{"Melange.exe", bin}, {"melange.asi", bin}, {"Melange.ini", kTemplate}, {"dinput8.dll", "loader"},
                {"tools/xomtool.exe", "xom"}, {"INSTALL.txt", "install"}, {"Mods/sample/spice.json", "{}"}});
}

void TestUpdateDownload() {
    const std::wstring root = Fresh(L"update-dl");
    const std::wstring updates = root + L"\\updates";
    const U::Signer dev;   // an unsigned launcher: signatures are not compared
    FakeRelease f = MakeRelease(root, "9.0.0", ReleaseZip());
    U::Source src;
    src.latestUrl = f.latestUrl;
    src.downloadPrefix = f.prefix;
    R::Release rel;
    std::string err;
    Expect(U::FetchLatest(src, &rel, &err) && rel.version == "9.0.0", "update: latest from a file:/// fixture", err);
    uint64_t lastGot = 0, lastTotal = 0;
    U::Staged st;
    const bool ok = U::Download(src, rel, updates, dev, [&](uint64_t got, uint64_t total) {
        lastGot = got;
        lastTotal = total;
    }, &st, &err);
    Expect(ok, "update: download, verify and extract", err);
    Expect(st.version == "9.0.0" && L::FileExists(st.payload + L"\\Melange.exe") && L::FileExists(st.payload + L"\\tools\\xomtool.exe") &&
               L::FileExists(st.payload + L"\\Mods\\sample\\spice.json") && L::FileExists(st.dir + L"\\ready.json") &&
               L::FileExists(st.dir + L"\\melange-9.0.0.zip") && !L::FileExists(st.dir + L"\\melange-9.0.0.zip.part"),
           "update: staged payload and ready.json");
    Expect(lastTotal == f.zip.size() && lastGot == lastTotal, "update: progress reaches the zip's size");
    U::Staged found;
    Expect(U::FindReady(updates, "0.3.6", &found) && found.version == "9.0.0" && found.htmlUrl == "https://github.com/JaminB/melange/releases/tag/v9.0.0",
           "update: FindReady finds it");
    Expect(!U::FindReady(updates, "9.0.0", &found) && !U::FindReady(updates, "10.0", &found), "update: nothing newer than the running version");
    // A second Download of the same version reuses the staged copy.
    DeleteFileW((f.dir + L"\\melange-9.0.0.zip").c_str());
    Expect(U::Download(src, rel, updates, dev, nullptr, &st, &err) && st.version == "9.0.0", "update: a staged release is reused", err);

    // Tampering: wrong hash, wrong size, the asset size disagreeing, binaries of another version, unsafe zips.
    struct Case {
        const char* what;
        FakeRelease f;
        const char* needle;
    };
    const std::wstring r2 = Fresh(L"update-bad");
    std::vector<Case> cases;
    cases.push_back({"update: sha256 mismatch", MakeRelease(r2 + L"\\a", "9.0.0", ReleaseZip(), std::string(64, 'b')), "SHA-256"});
    cases.push_back({"update: size mismatch", MakeRelease(r2 + L"\\b", "9.0.0", Zip({{"Melange.exe", "x"}}), "", 12345, 12345), "bytes"});
    cases.push_back({"update: asset size disagrees", MakeRelease(r2 + L"\\c", "9.0.0", ReleaseZip(), "", -1, 7), "manifest at"});
    cases.push_back({"update: binaries of another version", MakeRelease(r2 + L"\\d", "9.0.0", ReleaseZip(L"fake_asi_0_4_0.dll")), "not 9.0.0"});
    cases.push_back({"update: zip escapes its folder", MakeRelease(r2 + L"\\e", "9.0.0", Zip({{"../evil.txt", "x"}, {"Melange.exe", "x"}})), "plain relative"});
    cases.push_back({"update: zip with a dot segment", MakeRelease(r2 + L"\\f", "9.0.0", Zip({{"a/./evil.txt", "x"}})), "plain relative"});
    cases.push_back({"update: not a zip", MakeRelease(r2 + L"\\g", "9.0.0", "PK but not really"), "not a valid zip"});
    cases.push_back({"update: payload incomplete", MakeRelease(r2 + L"\\h", "9.0.0", Zip({{"Melange.exe", "x"}})), "has no"});
    for (auto& c : cases) {
        U::Source s2;
        s2.latestUrl = c.f.latestUrl;
        s2.downloadPrefix = c.f.prefix;
        R::Release r;
        U::Staged out;
        const std::wstring up = L::Parent(c.f.dir) + L"\\updates";
        const bool got = U::FetchLatest(s2, &r, &err) && U::Download(s2, r, up, dev, nullptr, &out, &err);
        Expect(!got && err.find(c.needle) != std::string::npos, c.what, err);
        Expect(!L::DirExists(up + L"\\9.0.0"), "update: a failed download leaves nothing staged", c.what);
        Expect(!L::FileExists(up + L"\\evil.txt") && !L::FileExists(up + L"\\9.0.0\\evil.txt"), "update: nothing written outside the payload", c.what);
    }
    // Asset URLs outside the allowed prefix are never fetched.
    U::Source wrong = src;
    wrong.downloadPrefix = "https://github.com/JaminB/melange/releases/download/";
    Expect(!U::Download(wrong, rel, root + L"\\updates-wrong", dev, nullptr, &st, &err) && err.find("not a Melange release download") != std::string::npos,
           "update: assets from elsewhere are refused", err);
    // A release without the manifest (published before the updater existed) is not offered.
    R::Release noManifest = rel;
    std::erase_if(noManifest.assets, [](const R::Asset& a) { return a.name.ends_with(".json"); });
    Expect(!U::Download(src, noManifest, root + L"\\updates-nm", dev, nullptr, &st, &err) && err.find("has no melange-9.0.0.json") != std::string::npos,
           "update: a release without its manifest is skipped", err);
    // Offline.
    U::Source offline;
    offline.latestUrl = FileUrl(root + L"\\nowhere\\latest.json");
    Expect(!U::FetchLatest(offline, &rel, &err) && err.find("could not reach GitHub") != std::string::npos, "update: offline is an error, not a crash", err);
}

void TestUpdateClean() {
    const std::wstring root = Fresh(L"update-clean");
    for (const wchar_t* v : {L"1.0.0", L"2.0.0", L"3.0.0", L"junk"}) Put(root + L"\\" + v + L"\\payload\\Melange.exe", "x");
    for (const wchar_t* v : {L"1.0.0", L"2.0.0"}) Put(root + L"\\" + v + L"\\ready.json", "{\"release\":\"" + L::Narrow(v) + "\"}");
    Put(root + L"\\3.0.0\\ready.json", "{\"release\":\"2.0.0\"}");   // names another version: not ready
    Put(root + L"\\result.json", "{}");
    U::Staged s;
    Expect(U::FindReady(root, "0.3.6", &s) && s.version == "2.0.0", "update: the newest ready version wins", s.version);
    Expect(U::FindReady(root, "1.5", &s) && s.version == "2.0.0" && !U::FindReady(root, "2.0.0", &s), "update: only newer than running");
    U::Clean(root, "2.0.0", root + L"\\1.0.0\\payload\\Melange.exe");
    Expect(L::DirExists(root + L"\\2.0.0") && L::DirExists(root + L"\\1.0.0") && !L::DirExists(root + L"\\3.0.0") && !L::DirExists(root + L"\\junk") &&
               L::FileExists(root + L"\\result.json"),
           "update: Clean keeps the latest, the one in use, and plain files");
    U::Clean(root, "", L"");
    Expect(!L::DirExists(root + L"\\2.0.0") && !L::DirExists(root + L"\\1.0.0"), "update: Clean with nothing to keep");
}

std::vector<std::wstring> Argv(const std::wstring& line) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(line.c_str(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.push_back(argv[i]);
    if (argv) LocalFree(argv);
    return args;
}

void TestUpdateArgs() {
    U::ApplyArgs a;
    a.from = L"C:\\Users\\me\\Downloads\\melange 0.3.6";
    a.game = L"D:\\Steam\\steamapps\\common\\WormsXHD";
    a.pid = 4242;
    std::vector<std::wstring> args = Argv(U::ApplyCommandLine(L"C:\\Users\\me\\AppData\\Local\\Melange\\updates\\0.3.7\\payload\\Melange.exe", a));
    U::ApplyArgs b;
    std::string err;
    Expect(U::IsApplyCommand(args) && U::ParseApplyArgs(args, &b, &err), "apply args: round trip parses", err);
    Expect(b.from == a.from && b.game == a.game && b.pid == 4242 && !b.elevated && b.result.empty(), "apply args: round trip values");
    // A drive root keeps its trailing backslash without eating the closing quote; elevated + result.
    a.from = L"E:\\";
    a.elevated = true;
    a.result = L"C:\\Temp\\r.json";
    args = Argv(L"x.exe " + U::ApplyCommandLine(L"", a));
    Expect(U::ParseApplyArgs(args, &b, &err) && b.from == L"E:\\" && b.elevated && b.result == L"C:\\Temp\\r.json", "apply args: drive root and flags",
           err + " " + L::Narrow(b.from));
    const std::vector<std::vector<std::wstring>> bad = {
        {L"--apply-update", L"--pid", L"1"},                                     // no --from
        {L"--apply-update", L"--from", L"C:\\x"},                               // no --pid
        {L"--apply-update", L"--from", L"relative\\dir", L"--pid", L"1"},       // relative
        {L"--apply-update", L"--from", L"C:\\x", L"--pid", L"12ab"},            // bad pid
        {L"--apply-update", L"--from", L"C:\\x", L"--pid", L"1", L"--bogus"},   // unknown
        {L"--from", L"C:\\x", L"--pid", L"1"},                                  // not the command
    };
    for (const auto& v : bad) Expect(!U::ParseApplyArgs(v, &b, &err), "apply args: refused", L::Narrow(v.back()));
    Expect(!U::IsApplyCommand({L"--game", L"C:\\x"}), "apply args: a normal start is not an apply");
}

void TestUpdateApply() {
    // The game has Melange 0.3.1 installed; the staged release is 0.4.0; the old Melange.exe ran from a third folder.
    Rig r = MakeRig(L"update-apply");
    const std::wstring origin = L::Parent(r.game) + L"\\origin";
    Copy(g_bin + L"\\fake_asi_0_3_1.dll", r.game + L"\\melange.asi");
    Copy(Ual(), r.game + L"\\dinput8.dll");
    Put(r.game + L"\\Melange.ini", "[Logging]\r\nEnabled=0\r\n");
    Put(r.game + L"\\Melange.exe", "old exe in game");
    Put(origin + L"\\Melange.exe", "old exe");
    Put(origin + L"\\melange.asi", "old asi");
    Put(origin + L"\\notes.txt", "mine");
    Expect(S::Inspect(r.ctx).melangeState == "older", "apply: the game has an older Melange");

    // The game running: refused, nothing changed anywhere.
    S::Context running = r.ctx;
    running.running = [](const std::wstring&) { return true; };
    const auto before = Snap(r.game), beforeOrigin = Snap(origin);
    U::ApplyOutcome o = U::ApplyStaged(running, origin);
    Expect(!o.ok && !o.needElevation && o.message.find("Close Worms") != std::string::npos, "apply: refused while the game runs", o.message);
    Expect(Snap(r.game) == before && Snap(origin) == beforeOrigin, "apply: a refusal changes nothing");

    o = U::ApplyStaged(r.ctx, origin);
    Expect(o.ok && o.gameUpdated && !o.backupId.empty(), "apply: applied", o.message);
    Expect(S::Inspect(r.ctx).melangeState == "installed" && Get(r.game + L"\\melange.asi") == Get(r.payload + L"\\melange.asi") &&
               Get(r.game + L"\\Melange.exe") == Get(r.payload + L"\\Melange.exe"),
           "apply: the game folder has the new Melange and Melange.exe");
    Expect(Get(r.game + L"\\Melange.ini").find("Enabled=0") != std::string::npos && Get(r.game + L"\\Melange.ini").find("Verbose=0") != std::string::npos,
           "apply: the user's settings kept, new keys merged");
    Expect(L::FileExists(r.game + L"\\Melange\\backup\\" + L::Widen(o.backupId) + L"\\melange.asi"), "apply: the old melange.asi is backed up");
    Expect(Get(origin + L"\\Melange.exe") == Get(r.payload + L"\\Melange.exe") && Get(origin + L"\\Melange.exe.old") == "old exe" &&
               Get(origin + L"\\melange.asi") == Get(r.payload + L"\\melange.asi") && Get(origin + L"\\notes.txt") == "mine" &&
               !L::FileExists(origin + L"\\Melange.ini"),
           "apply: the old exe's folder is refreshed, Melange.exe.old kept, other files left alone");
    U::DeleteOldExe(origin);
    Expect(!L::FileExists(origin + L"\\Melange.exe.old"), "apply: Melange.exe.old is deleted on the next start");

    // Applying again (the elevated retry runs the whole thing a second time) is harmless.
    o = U::ApplyStaged(r.ctx, origin);
    Expect(o.ok && o.backupId.empty(), "apply: a second apply changes nothing", o.message);

    // Melange uninstalled from the game folder: only the exe's folder is updated, with a warning.
    Rig m = MakeRig(L"update-apply-missing");
    const std::wstring origin2 = L::Parent(m.game) + L"\\origin";
    Put(origin2 + L"\\Melange.exe", "old exe");
    o = U::ApplyStaged(m.ctx, origin2);
    Expect(o.ok && !o.gameUpdated && o.warnings.size() == 1 && !L::FileExists(m.game + L"\\melange.asi") &&
               Get(origin2 + L"\\Melange.exe") == Get(m.payload + L"\\Melange.exe"),
           "apply: no install in the game folder -> only Melange.exe", o.message);
    // No game folder at all.
    S::Context none = m.ctx;
    none.gameDir.clear();
    Put(origin2 + L"\\Melange.exe", "old again");
    Expect(U::ApplyStaged(none, origin2).ok && Get(origin2 + L"\\Melange.exe") == Get(m.payload + L"\\Melange.exe"), "apply: no game folder chosen");

    // The result handed to the next start, once.
    const std::wstring res = Fresh(L"update-result") + L"\\result.json";
    U::Result w;
    w.present = w.ok = w.gameUpdated = true;
    w.version = "0.4.0";
    w.at = "2026-10-05T10:00:00Z";
    w.warnings = {"a \"quoted\" warning"};
    Expect(U::WriteResult(res, w), "result: written");
    U::Result t;
    Expect(U::TakeResult(res, &t) && t.present && t.ok && t.gameUpdated && t.version == "0.4.0" && t.warnings.size() == 1 &&
               t.warnings[0] == "a \"quoted\" warning",
           "result: read back");
    Expect(!L::FileExists(res) && !U::TakeResult(res, &t) && !t.present, "result: shown once");
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
    TestLaa();
    TestDll();
    if (L::FileExists(Ual())) {
        TestEngineFresh();
        TestEngineLoaders();
        TestEngineCleanup();
        TestEngineGuards();
        TestEngineRollback();
        TestVanilla();
        TestVanillaLink();
        TestUpdateApply();
    }
    TestStockList();
    TestIniMerge();
    TestPluginSettings();
    TestRecommended();
    TestSettings();
    TestInGameCheckSync();
    TestBusyStatusJson();
    TestStoreEngine();
    TestUpdateRelease();
    TestUpdateSigner();
    TestUpdateDownload();
    TestUpdateClean();
    TestUpdateArgs();
    TestStoreReconcile();
    melange::store::Shutdown();
    Wipe(g_tmp);
    printf("launcher_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
