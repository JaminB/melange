// launcher.laa.*: Settings > Game > "Use up to 4 GB of memory". [Game] LargeAddressAware in Melange.ini is the
// choice; WormsMayhem.exe's header follows it (setup/laa.h), here when the page asks and again at every Launch.
#include <windows.h>

#include <mutex>

#include "core/pe_laa.h"
#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/setup/laa.h"
#include "launcher/setup/running.h"
#include "launcher/util.h"
#include "tools/json_mini.h"

namespace melange::launcher::rpc {
namespace {
using oasis::Call;
using oasis::Result;

// {enabled: the ini asks for it, active: the exe has the bit, byMelange: Melange set it, running, melangeIni,
// refused?: why the toggle is off right now}.
std::string StateJson(const std::wstring& game) {
    jsonmini::Obj o;
    o.Bool("enabled", !game.empty() && setup::IniWantsLaa(game))
        .Bool("active", !game.empty() && pe::IsLaaFile(game + L"\\WormsMayhem.exe"))
        .Bool("byMelange", setup::LaaMarkerPresent(game))
        .Bool("melangeIni", !game.empty() && FileExists(game + L"\\Melange.ini"))
        .Bool("running", !game.empty() && setup::GameRunning(game));
    const std::string gate = app::WriteGate();
    if (!gate.empty()) o.Str("refused", gate);
    return o.End();
}

void Get(const Call&, Result& r, void*) { r.json = StateJson(app::GameDir()); }

// {enabled}: writes the ini, then makes the exe match. Refused while the game runs.
void Set(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const json::Value* en = p.Get("enabled");
    if (!en || !en->IsBool()) return Fail(r, -32602, "expected {enabled}");
    const std::wstring game = app::GameDir();
    if (const std::string gate = app::WriteGate(); !gate.empty()) return Fail(r, -32000, gate);
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    if (!FileExists(game + L"\\Melange.ini")) return Fail(r, -32000, "Install Melange first: the 4 GB option is one of its settings.");
    const bool before = setup::IniWantsLaa(game);
    if (const std::string err = setup::SetIniLaa(game, en->boolean); !err.empty()) return Fail(r, -32000, err);
    // Turning it off only undoes what Melange did; an exe patched by something else stays as it is.
    const setup::LaaResult res = setup::EnsureLaa(app::MakeContext(), en->boolean);
    if (!res.ok) {
        setup::SetIniLaa(game, before);
        return Fail(r, res.needsAdmin ? -32010 : -32000, res.message);
    }
    lk.unlock();
    r.json = StateJson(game);
}
}  // namespace

void InstallLaa() {
    oasis::AddMethod("launcher.laa.get", &Get, nullptr, oasis::kRpcServerThread);
    oasis::AddMethod("launcher.laa.set", &Set, nullptr, oasis::kRpcServerThread | oasis::kRpcMutating);
}
}  // namespace melange::launcher::rpc
