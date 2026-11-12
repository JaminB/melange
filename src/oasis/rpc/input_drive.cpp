// input.drive: hold the engine's own movement, jump, aim and fire inputs for a test tool (erg-mcp's game_walk and test
// {fire}), through the Controls module's drive (controls::Drive): each input is posted as the key bound to it posts it,
// <name>Pressed, then <name>Released at its deadline, from the input service's tick, so it works whatever window has
// the focus (Automation's injected keys go through DirectInput, which the game reads only with the focus). Offline
// matches only. Main thread.
#include <string>
#include <vector>

#include "input/controls.h"
#include "oasis/providers.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;

std::string StateJson() {
    const controls::DriveState st = controls::DriveNow();
    jsonmini::Arr held;
    for (const controls::DriveHeld& h : st.held)
        held.Raw(jsonmini::Obj().Str("input", h.name).Bool("posted", h.posted).Int("msLeft", h.msLeft).End());
    jsonmini::Obj o;
    o.Raw("held", held.End()).UInt("posts", st.posts);
    if (!st.lastError.empty()) o.Str("lastError", st.lastError);
    return o.End();
}

// input.drive {hold: ["Input.MoveForward" | "MoveForward", ...], ms?: 1-5000 (default 250), replace?: true} |
// {release: true} | {} (the state)
void Drive(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    if (!controls::Available()) {
        Fail(r, rpc::kRefused, "the Controls module is not installed ([Controls] Enabled, a known game build)");
        return;
    }
    bool release = false;
    if (const json::Value* v = p.Get("release"); v && !v->IsNull() && !rpc::Flag(p, "release", &release, r)) return;
    const json::Value* hold = p.Get("hold");
    std::vector<std::string> names;
    if (hold && !hold->IsNull()) {
        if (!hold->IsArray() || hold->items.size() > 12) {
            Fail(r, rpc::kBadParams, "hold: an array of at most 12 input names");
            return;
        }
        for (const json::Value& n : hold->items) {
            if (!n.IsString() || n.string.empty() || n.string.size() > 40) {
                Fail(r, rpc::kBadParams, "hold: input names such as Input.MoveForward (or MoveForward)");
                return;
            }
            names.push_back(n.string.rfind("Input.", 0) == 0 ? n.string : "Input." + n.string);
        }
    }
    if (release && !names.empty()) {
        Fail(r, rpc::kBadParams, "give hold or release, not both");
        return;
    }
    if (release || !names.empty()) {
        double ms = 250;
        if (const json::Value* v = p.Get("ms"); v && !v->IsNull()) {
            if (!v->IsNumber() || v->number < 1 || v->number > 5000) {
                Fail(r, rpc::kBadParams, "ms must be a number from 1 to 5000");
                return;
            }
            ms = v->number;
        }
        bool replace = release;
        if (const json::Value* v = p.Get("replace"); v && !v->IsNull() && !rpc::Flag(p, "replace", &replace, r)) return;
        std::string why;
        if (!controls::Drive(names, static_cast<int>(ms), replace || release, &why)) {
            Fail(r, rpc::kRefused, why);
            return;
        }
    }
    r.json = StateJson();
}
}  // namespace

void InstallInputDrive() { AddMethod("input.drive", &Drive, nullptr, kRpcMutating | kRpcGameOnly); }
}  // namespace melange::oasis::providers
