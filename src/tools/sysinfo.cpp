// The in-game half of tools/sysinfo.h: GL from the overlay and the exe as the running game identified it.
#include "tools/sysinfo.h"

#include <windows.h>

#include "core/game.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "melange/overlay.h"

namespace melange::sysinfo {

std::vector<PluginFile> DetectPlugins() { return DetectPluginsIn(melange::game::GameDir()); }

std::string PluginsJson() { return PluginsJsonIn(melange::game::GameDir()); }

std::string CollectJson() {
    // gl.valid stays false until the overlay has seen a GL context.
    melange::overlay::GlInfo gl = melange::overlay::Gl();
    jsonmini::Obj glj;
    glj.Bool("valid", gl.valid)
        .Str("vendor", gl.vendor)
        .Str("renderer", gl.renderer)
        .Str("version", gl.version)
        .Str("glsl", gl.glsl)
        .Int("viewportW", gl.viewportW)
        .Int("viewportH", gl.viewportH);

    const melange::game::ExeInfo& exe = melange::game::Exe();
    std::wstring exePath = melange::game::GameDir().empty() ? std::wstring() : melange::game::GameDir() + L"\\WormsMayhem.exe";
    jsonmini::Obj exej;
    exej.Str("path", melange::game::Narrow(exePath))
        .UInt("size", exe.fileSize)
        .UInt("timestampUtc", exe.timestamp)
        .Str("sha256", exe.sha256)   // canonical: the large-address-aware bit cleared
        .Str("rawSha256", exePath.empty() ? std::string() : melange::hashutil::Sha256HexFile(exePath))
        .Bool("largeAddressAware", exe.laa)
        .Str("build", exe.build)
        .Bool("known", exe.known);

    return CollectJsonWith(melange::game::GameDir(), glj.End(), exej.End());
}
}  // namespace melange::sysinfo
