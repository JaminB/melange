// display.*: Settings › Display. The window size the game opens at (local.cfg /W /H, which the engine reads after
// Default.cfg) and Melange's borderless fullscreen ([Display] Fullscreen, applied by melange.asi as soon as the game's
// window is up). Written before launch only: both files are read once at the game's start.
#include <windows.h>

#include <mutex>

#include "core/log.h"
#include "launcher/app.h"
#include "launcher/local_cfg.h"
#include "launcher/rpc.h"
#include "launcher/setup/running.h"
#include "launcher/util.h"
#include "oasis/rpc/ini_edit.h"
#include "oasis/standalone/register.h"
#include "tools/json_mini.h"

namespace melange::launcher::rpc {
namespace {
using oasis::Call;
using oasis::Result;
namespace lc = localcfg;

// The primary monitor (the game opens there) in physical pixels (Melange.exe is per-monitor DPI aware), and the
// display modes Windows reports for it.
struct Monitor {
    int w = 0, h = 0;
    std::vector<lc::Size> modes;
};
Monitor PrimaryMonitor() {
    Monitor m;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) return m;
    m.w = mi.rcMonitor.right - mi.rcMonitor.left;
    m.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm) && i < 4096; ++i)
        m.modes.push_back({static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight)});
    return m;
}

std::string ReadText(const std::wstring& path) {
    std::string s;
    if (!path.empty() && FileExists(path)) ReadAll(path, &s, 64u << 10);
    return s;
}

struct Current {
    lc::Info info;          // local.cfg over Default.cfg
    std::string source;     // "local", "default" or "none": where the size came from
    bool localCfg = false;  // local.cfg exists
    bool iniPresent = false, fullscreen = false, enabled = true;
    std::string hotkey;
};
Current ReadCurrent(const std::wstring& game) {
    Current c;
    const std::string local = ReadText(game.empty() ? std::wstring() : game + L"\\local.cfg");
    const lc::Info def = lc::Read(ReadText(game.empty() ? std::wstring() : game + L"\\Default.cfg"));
    const lc::Info loc = lc::Read(local);
    c.localCfg = !game.empty() && FileExists(game + L"\\local.cfg");
    c.info = def;
    c.source = def.w > 0 && def.h > 0 ? "default" : "none";
    if (loc.w > 0 && loc.h > 0) {
        c.info.w = loc.w;
        c.info.h = loc.h;
        c.source = "local";
    }
    if (loc.fs || loc.win) {
        c.info.fs = loc.fs;
        c.info.win = loc.win;
    }
    c.iniPresent = !game.empty() && FileExists(game + L"\\Melange.ini");
    namespace sa = oasis::standalone;
    c.fullscreen = strtol(sa::IniGet(game, "Display", "Fullscreen", "0").c_str(), nullptr, 0) != 0;
    c.enabled = strtol(sa::IniGet(game, "Display", "Enabled", "1").c_str(), nullptr, 0) != 0;
    c.hotkey = sa::IniGet(game, "Display", "Hotkey", "Alt+RETURN");
    return c;
}

std::string SizeJson(int w, int h) {
    jsonmini::Obj o;
    o.Int("w", w).Int("h", h);
    return o.End();
}

std::string StateJson(const std::wstring& game, bool removedFs) {
    const Monitor m = PrimaryMonitor();
    const Current c = ReadCurrent(game);
    jsonmini::Arr modes;
    for (const lc::Size& s : lc::Modes(m.modes, m.w, m.h)) modes.Raw(SizeJson(s.w, s.h));
    jsonmini::Obj o;
    o.Raw("monitor", SizeJson(m.w, m.h)).Raw("modes", modes.End());
    if (c.info.w > 0 && c.info.h > 0) o.Raw("windowed", SizeJson(c.info.w, c.info.h));
    else o.Raw("windowed", "null");
    o.Str("source", c.source).Bool("localCfg", c.localCfg).Bool("exclusive", c.info.fs).Bool("fullscreen", c.fullscreen)
        .Bool("enabled", c.enabled).Str("hotkey", c.hotkey).Bool("melangeIni", c.iniPresent)
        .Bool("running", !game.empty() && setup::GameRunning(game));
    const std::string gate = app::WriteGate();
    if (!gate.empty()) o.Str("refused", gate);
    if (removedFs) o.Bool("removedFs", true);
    return o.End();
}

void Get(const Call&, Result& r, void*) { r.json = StateJson(app::GameDir(), false); }

bool WriteIni(const std::wstring& game, bool fullscreen, std::string* err) {
    const std::wstring path = game + L"\\Melange.ini";
    std::string bytes;
    if (!ReadAll(path, &bytes, 4u << 20)) return *err = "Melange.ini could not be read.", false;
    oasis::ini::Encoding enc{};
    std::string text = oasis::ini::Decode(bytes, &enc);
    const auto entries = oasis::ini::Parse(text);
    const oasis::ini::Entry* e = oasis::ini::Find(entries, "Display", "Fullscreen");
    const oasis::ini::Entry* en = oasis::ini::Find(entries, "Display", "Enabled");
    const bool now = e && strtol(e->value.c_str(), nullptr, 0) != 0;
    const bool off = en && !en->value.empty() && strtol(en->value.c_str(), nullptr, 0) == 0;
    if (now == fullscreen && !(fullscreen && off)) return true;
    text = oasis::ini::Set(text, "Display", "Fullscreen", fullscreen ? "1" : "0");
    // Turning fullscreen on is asking for the module: a [Display] Enabled=0 would leave it doing nothing.
    if (fullscreen && off) text = oasis::ini::Set(text, "Display", "Enabled", "1");
    std::string out;
    if (!oasis::ini::Encode(text, enc, &out)) return *err = "Melange.ini could not be written in its encoding.", false;
    if (const unsigned long w = WriteAtomic(path, out)) return *err = "Could not write Melange.ini: " + Win32Message(w), false;
    LOG_INFO("[display] Melange.ini: [Display] Fullscreen=%d%s", fullscreen ? 1 : 0, fullscreen && off ? " (and Enabled=1)" : "");
    return true;
}

// {fullscreen, width, height}: local.cfg gets /W /H (and loses /FS when fullscreen is on: the engine's exclusive mode
// and Melange's borderless one conflict), Melange.ini gets [Display] Fullscreen. Refused while the game runs.
void Set(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const json::Value* fs = p.Get("fullscreen");
    const json::Value* w = p.Get("width");
    const json::Value* h = p.Get("height");
    if (!fs || !fs->IsBool() || !w || !w->IsNumber() || !h || !h->IsNumber()) return Fail(r, -32602, "expected {fullscreen, width, height}");
    const int width = static_cast<int>(w->number), height = static_cast<int>(h->number);
    if (!lc::ValidSize(width, height) || width != w->number || height != h->number)
        return Fail(r, -32602, "the window size must be whole pixels, at least 640x480");
    const std::wstring game = app::GameDir();
    if (const std::string gate = app::WriteGate(); !gate.empty()) return Fail(r, -32000, gate);
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    const bool iniPresent = FileExists(game + L"\\Melange.ini");
    if (fs->boolean && !iniPresent) return Fail(r, -32000, "Install Melange first: fullscreen is one of its features.");

    const std::wstring cfg = game + L"\\local.cfg";
    const std::string before = ReadText(cfg);
    bool removedFs = false;
    const std::string after = lc::Rewrite(before, width, height, fs->boolean, &removedFs);
    if (after != before || !FileExists(cfg)) {
        if (const unsigned long e = WriteAtomic(cfg, after)) return Fail(r, -32000, "Could not write local.cfg: " + Win32Message(e));
        LOG_INFO("[display] local.cfg: %s", after.c_str());
    }
    std::string err;
    if (iniPresent && !WriteIni(game, fs->boolean, &err)) return Fail(r, -32000, err);
    lk.unlock();
    r.json = StateJson(game, removedFs);
}
}  // namespace

void InstallDisplay() {
    oasis::AddMethod("display.get", &Get, nullptr, oasis::kRpcServerThread);
    oasis::AddMethod("display.set", &Set, nullptr, oasis::kRpcServerThread | oasis::kRpcMutating);
}
}  // namespace melange::launcher::rpc
