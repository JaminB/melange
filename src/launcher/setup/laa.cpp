#include "launcher/setup/laa.h"

#include <windows.h>

#include "core/log.h"
#include "core/pe_laa.h"
#include "launcher/setup/running.h"
#include "launcher/util.h"
#include "oasis/rpc/ini_edit.h"
#include "tools/json_mini.h"

namespace melange::launcher::setup {
namespace {
constexpr char kRunningCopy[] = "Close Worms Ultimate Mayhem first.";

std::wstring MarkerPath(const std::wstring& g) { return g + L"\\Melange\\laa.json"; }

LaaResult Refused(std::string why) {
    LaaResult r;
    r.state = "refused";
    r.message = std::move(why);
    return r;
}

LaaResult Failed(const char* what, unsigned long e) {
    LaaResult r;
    r.state = "failed";
    r.win32 = e;
    r.needsAdmin = e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD;
    r.message = r.needsAdmin ? "Windows didn't let us change WormsMayhem.exe. Run Melange as administrator and try again."
                             : std::string(what) + " Windows said: " + Win32Message(e);
    return r;
}

const char* Bit(bool on) { return on ? "on" : "off"; }
}  // namespace

bool LaaMarkerPresent(const std::wstring& g) { return !g.empty() && FileExists(MarkerPath(g)); }

bool IniWantsLaa(const std::wstring& g) {
    std::string bytes;
    if (g.empty() || !ReadAll(g + L"\\Melange.ini", &bytes, 4u << 20)) return false;
    oasis::ini::Encoding enc{};
    const auto entries = oasis::ini::Parse(oasis::ini::Decode(bytes, &enc));
    const oasis::ini::Entry* e = oasis::ini::Find(entries, "Game", "LargeAddressAware");
    return e && strtol(e->value.c_str(), nullptr, 0) != 0;
}

std::string SetIniLaa(const std::wstring& g, bool on) {
    const std::wstring path = g + L"\\Melange.ini";
    std::string bytes;
    if (!ReadAll(path, &bytes, 4u << 20)) return "Melange.ini could not be read.";
    oasis::ini::Encoding enc{};
    std::string text = oasis::ini::Decode(bytes, &enc);
    if (IniWantsLaa(g) == on && oasis::ini::Find(oasis::ini::Parse(text), "Game", "LargeAddressAware")) return {};
    text = oasis::ini::Set(text, "Game", "LargeAddressAware", on ? "1" : "0");
    std::string out;
    if (!oasis::ini::Encode(text, enc, &out)) return "Melange.ini could not be written in its encoding.";
    if (const unsigned long w = WriteAtomic(path, out)) return "Could not write Melange.ini: " + Win32Message(w);
    LOG_INFO("[laa] Melange.ini: [Game] LargeAddressAware=%d", on ? 1 : 0);
    return {};
}

LaaResult EnsureLaa(const Context& ctx, bool want, bool force) {
    const std::wstring g = ctx.gameDir;
    if (g.empty()) return Refused("Choose your game folder first.");
    if (ctx.running ? ctx.running(g) : GameRunning(g)) return Refused(kRunningCopy);
    const std::wstring exe = g + L"\\WormsMayhem.exe", tmp = exe + L".melange-tmp";
    DeleteFileW(tmp.c_str());   // a swap that was cut short by a crash or a power loss

    // Only the supported build: its size, timestamp and canonical hash all match a known profile.
    const GameCheck chk = CheckExe(g, ctx.profiles ? *ctx.profiles : DefaultProfiles());
    if (chk.verdict == Verdict::Unreadable) return Failed("WormsMayhem.exe could not be read.", ERROR_SHARING_VIOLATION);
    if (chk.verdict != Verdict::Ok) return Refused("The 4 GB option only works with the supported game version (Steam/GOG, build #1077).");

    uint16_t chars = 0;
    if (!pe::ReadPeFlags(exe, &chars)) return Failed("WormsMayhem.exe could not be read.", ERROR_BAD_EXE_FORMAT);
    const bool now = (chars & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    bool marker = LaaMarkerPresent(g);
    if (!now && marker) {
        // The marker only means something while the bit is set: Steam's "Verify files", a crash before the swap or a
        // failed delete leave it behind, and it must never later pass for ownership of someone else's patch.
        if (DeleteFileW(MarkerPath(g).c_str())) marker = false;
        else LOG_WARN("[laa] could not remove the stale marker: %s", Win32Message(GetLastError()).c_str());
    }
    LaaResult r;
    r.ok = true;
    if (now == want) {
        r.state = "unchanged";
        return r;
    }
    if (!want && !marker && !force) {
        // Someone else set it (another patcher, 4GB Patch): not ours to undo.
        r.state = "external";
        return r;
    }

    // The marker goes down first: a crash after the swap must not leave a patched exe Melange would not revert.
    bool wroteMarker = false;
    if (want && !marker) {
        MakeDirs(g + L"\\Melange");
        jsonmini::Obj o;
        char oc[16];
        snprintf(oc, sizeof oc, "0x%04x", chars);
        o.Str("appliedBy", "melange").Str("originalCharacteristics", oc).Str("at", NowIsoUtc()).Str("melange", ctx.version);
        if (const unsigned long e = WriteAtomic(MarkerPath(g), o.End())) return Failed("Could not record the change.", e);
        wroteMarker = true;
    }
    auto fail = [&](const char* what, unsigned long e) {
        DeleteFileW(tmp.c_str());
        if (wroteMarker) DeleteFileW(MarkerPath(g).c_str());
        return Failed(what, e);
    };

    // Copy, patch the copy, prove it, then swap it in. The exe itself is only ever replaced by a checked file.
    if (!CopyFileW(exe.c_str(), tmp.c_str(), FALSE)) return fail("Could not copy WormsMayhem.exe.", GetLastError());
    if (const unsigned long e = pe::SetLaaInFile(tmp, want)) return fail("Could not patch the copy of WormsMayhem.exe.", e);
    uint16_t after = 0;
    if (!pe::ReadPeFlags(tmp, &after) || ((after & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0) != want ||
        pe::CanonicalSha256(tmp) != chk.exe.sha256 || FileSize(tmp) != chk.exe.size)
        return fail("The patched copy did not check out, so WormsMayhem.exe was left alone.", ERROR_CRC);
    if (!MoveFileExW(tmp.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail("Could not replace WormsMayhem.exe.", GetLastError());

    if (!want && !DeleteFileW(MarkerPath(g).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        LOG_WARN("[laa] could not remove the marker after the revert: %s", Win32Message(GetLastError()).c_str());
    ClearExeCache();
    LOG_INFO("[laa] WormsMayhem.exe: large-address-aware %s (was %s)", Bit(want), Bit(now));
    r.changed = true;
    r.state = want ? "applied" : "reverted";
    return r;
}
}  // namespace melange::launcher::setup
