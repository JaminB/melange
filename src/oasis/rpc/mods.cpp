// mods.list, mods.setEnabled, mods.revokeDeepDesert and the `mods` channel. The page has the overlay's Mods page
// powers except one: Deep Desert can be revoked here but granted only in the game.
#include <string>
#include <vector>

#include "core/game.h"
#include "lua/sandbox_internal.h"
#include "melange/mods.h"
#include "mods/thumper_internal.h"
#include "oasis/providers.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;

ChannelId g_channel = 0;

const char* StateName(mods::State s) {
    switch (s) {
        case mods::State::Enabled: return "enabled";
        case mods::State::Disabled: return "disabled";
        case mods::State::Blocked: return "blocked";
        case mods::State::PendingConsent: return "pending-consent";
        case mods::State::Incompatible: return "incompatible";
        case mods::State::RestartRequired: return "restart-required";
    }
    return "unknown";
}

std::string Info(const mods::ModInfo& m) {
    const auto& pref = thumper::Live().enabled;
    const auto it = pref.find(m.id ? m.id : "");
    const bool on = it != pref.end() ? it->second : m.state == mods::State::Enabled;
    jsonmini::Obj o;
    o.Str("id", m.id ? m.id : "").Str("name", m.name ? m.name : "").Str("version", m.version ? m.version : "");
    o.Str("authors", m.authors ? m.authors : "").Str("dir", m.dir ? game::Narrow(m.dir) : "");
    o.Str("kind", m.kind == mods::Kind::Content ? "content" : "client").Str("state", StateName(m.state));
    o.Str("reason", m.reason ? m.reason : "").Bool("on", on).Bool("restartRequired", m.state == mods::State::RestartRequired);
    o.Bool("implicitManifest", m.implicitManifest).Bool("hasClient", m.hasClient).Bool("hasSim", m.hasSim);
    o.Raw("deepDesert", jsonmini::Obj().Bool("declared", m.unsafe).Bool("granted", m.unsafe && m.unsafeGranted).End());
    o.Int("order", m.order);
    sandbox::ModStatus st;
    if (m.id && sandbox::Status(m.id, &st)) {
        o.Raw("sandbox", jsonmini::Obj()
                             .Bool("loaded", st.loaded)
                             .Str("error", st.error)
                             .UInt("callbacks", st.callbacks)
                             .UInt("disabledCallbacks", st.disabledCallbacks)
                             .UInt("faults", st.faults)
                             .UInt("bytes", st.bytes)
                             .End());
    }
    return o.End();
}

std::string ListJson() {
    std::vector<mods::ModInfo> all(static_cast<size_t>(mods::List(nullptr, 0)));
    const int n = all.empty() ? 0 : mods::List(all.data(), static_cast<int>(all.size()));
    jsonmini::Arr a;
    for (int i = 0; i < n; ++i) a.Raw(Info(all[static_cast<size_t>(i)]));
    return a.End();
}

bool FindMod(const json::Value& p, std::string* id, mods::ModInfo* m, Result& r) {
    if (!rpc::Str(p, "id", id, r)) return false;
    if (!mods::Find(id->c_str(), m)) return Fail(r, rpc::kBadParams, "no mod '" + *id + "'");
    return true;
}

bool Reply(const std::string& id, Result& r) {
    mods::ModInfo m{};
    if (!mods::Find(id.c_str(), &m)) return Fail(r, rpc::kRefused, "mod '" + id + "' disappeared");
    r.json = Info(m);
    return true;
}

void List(const Call&, Result& r, void*) { r.json = ListJson(); }

void SetEnabled(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    mods::ModInfo m{};
    bool on = false;
    if (!rpc::ParseParams(c, &p, r) || !FindMod(p, &id, &m, r) || !rpc::Flag(p, "on", &on, r)) return;
    if (!mods::SetEnabled(id.c_str(), on)) return (void)Fail(r, rpc::kRefused, "Thumper refused the change");
    Reply(id, r);
}

void RevokeDeepDesert(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    mods::ModInfo m{};
    if (!rpc::ParseParams(c, &p, r) || !FindMod(p, &id, &m, r)) return;
    if (!m.unsafe) return (void)Fail(r, rpc::kBadParams, "mod '" + id + "' does not ask for Deep Desert");
    if (m.unsafeGranted && !mods::SetDeepDesert(id.c_str(), false)) return (void)Fail(r, rpc::kRefused, "Thumper refused the change");
    Reply(id, r);
}

void OnChanged(void*) {
    if (HasSubscribers(g_channel)) Publish(g_channel, ListJson());
}

void OnSub(ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (subscribed) PublishTo(ch, client, ListJson());
}
}  // namespace

void InstallMods() {
    ChannelOptions o;
    o.overflow = Overflow::Coalesce;
    o.mainThreadSubscribe = true;
    g_channel = AddChannel("mods", o);
    OnSubscribe(g_channel, &OnSub, nullptr);
    mods::OnChange(&OnChanged, nullptr);
    AddMethod("mods.list", &List, nullptr);
    AddMethod("mods.setEnabled", &SetEnabled, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("mods.revokeDeepDesert", &RevokeDeepDesert, nullptr, kRpcMutating | kRpcGameOnly);
}
}  // namespace melange::oasis::providers
