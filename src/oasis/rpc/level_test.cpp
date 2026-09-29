// level.test and the `erg` channel: building a project into the Test workspace, arming the one-shot override, and
// (E0 GO) starting Quick Game itself. The workspace build and the project store are component A; until it merges,
// this refuses with a clear reason, the same way S's stubs do for the other components' surfaces.
#include <string>

#include "erg/names.h"
#include "erg/quickstart.h"
#include "levels/engine.h"
#include "melange/levels.h"
#include "mods/lobby.h"
#include "oasis/providers.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;

ChannelId g_channel = 0;

const char* StateName(levels::TestState s) {
    switch (s) {
        case levels::TestState::Idle: return "idle";
        case levels::TestState::Registering: return "registering";
        case levels::TestState::Registered: return "registered";
        case levels::TestState::Armed: return "armed";
        case levels::TestState::Starting: return "starting";
        case levels::TestState::Playing: return "playing";
        case levels::TestState::Ended: return "ended";
        case levels::TestState::Failed: return "failed";
    }
    return "unknown";
}

void OnTestState(levels::TestState s, const char* key, const char* detail, void*) {
    if (!HasSubscribers(g_channel)) return;
    Publish(g_channel, jsonmini::Obj().Str("state", StateName(s)).Str("key", key ? key : "").Str("detail", detail ? detail : "").End());
}

bool InLobby() { return handshake::lobby::Current() != 0; }

// Builds the project into <game>\Melange\erg\test\ergtest_<project>.* (§3.6) and deletes its stale .csh. This is
// component A's project store and scene builder, not part of this version yet: there is nothing to read a patch
// from, so nothing is written and nothing is registered. Replace this with A's Open + LoadScene/ApplyPatch + Build
// once it merges; the stem below is already the frozen one and needs no change.
bool BuildTestWorkspace(const std::string& project, std::string* title, std::string* err) {
    (void)title;
    *err = "the level service is not built into this version (project '" + project + "')";
    return false;
}

void Test(const Call& c, Result& r, void*) {
    json::Value p;
    std::string project;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "project", &project, r)) return;
    if (!erg::names::ValidSlug(project)) {
        Fail(r, rpc::kBadParams, "project must match [a-z0-9]{1,24}");
        return;
    }
    if (InLobby()) {
        Fail(r, rpc::kRefused, "Test cannot start while you are in a lobby");
        return;
    }
    if (!levels::engine::AtFrontend()) {
        Fail(r, rpc::kNotInMatch, "the game must be at the frontend to test a level");
        return;
    }
    const std::string stem = std::string(erg::names::kTestPrefix) + "_" + project;
    const std::string key = erg::names::Key(stem);
    std::string title, buildErr;
    if (!BuildTestWorkspace(project, &title, &buildErr)) {
        Fail(r, rpc::kRefused, buildErr);
        return;
    }
    char regErr[256] = {};
    if (!levels::RegisterTest(stem.c_str(), title.c_str(), regErr, sizeof regErr)) {
        Fail(r, rpc::kRefused, regErr);
        return;
    }
    if (!levels::ArmNextLevel(key.c_str(), 120)) {
        Fail(r, rpc::kRefused, "could not arm the Test override");
        return;
    }
    const bool started = erg::quickstart::Available() && erg::quickstart::PostQuickGame();
    r.json = jsonmini::Obj().Str("key", key).Str("state", started ? "starting" : "armed").End();
}
}  // namespace

void InstallLevelTest() {
    ChannelOptions o;
    o.overflow = Overflow::Coalesce;
    g_channel = AddChannel("erg", o);
    levels::OnTestState(&OnTestState, nullptr);
    AddMethod("level.test", &Test, nullptr, kRpcMutating | kRpcGameOnly);
}
}  // namespace melange::oasis::providers
