// store.* methods, the `store` channel and the /store/shots/<id>/<n> route.
#include <string>
#include <vector>

#include "melange/oasis.h"
#include "mods/spice.h"
#include "oasis/core/server.h"
#include "oasis/rpc/params.h"
#include "store/store.h"
#include "tools/json_mini.h"

namespace melange::store {
namespace {
using oasis::Call;
using oasis::Result;
namespace rpc = oasis::rpc;

oasis::ChannelId g_channel = 0;

std::string Strings(const std::vector<std::string>& v) {
    jsonmini::Arr a;
    for (const std::string& s : v) a.Str(s);
    return a.End();
}

std::string Deps(const std::vector<Dep>& v) {
    jsonmini::Arr a;
    for (const Dep& d : v) a.Raw(jsonmini::Obj().Str("id", d.id).Str("range", d.range).End());
    return a.End();
}

void ItemFields(jsonmini::Obj& o, const Item& it) {
    o.Str("id", it.id).Str("name", it.name).Raw("authors", Strings(it.authors)).Str("description", it.description);
    o.Raw("categories", Strings(it.categories)).Str("kind", it.kind).Bool("unsafe", it.unsafe).Str("licence", it.licence);
    o.Str("latest", it.latest).Str("compatible", it.compatible).UInt("size", it.size);
    if (it.installed)
        o.Raw("installed", jsonmini::Obj().Str("version", it.installedVersion).Bool("managed", it.managed)
                               .Str("state", it.installedState).Bool("enabled", it.enabled).End());
    else
        o.Raw("installed", "null");
    o.Str("action", ActionName(it.action)).Bool("canRemove", it.canRemove).Str("state", it.state).Str("reason", it.reason);
    o.Str("error", it.error);
}

std::string ItemJson(const Item& it) {
    jsonmini::Obj o;
    ItemFields(o, it);
    return o.End();
}

std::string StatusJson(const Status& s) {
    jsonmini::Obj o;
    o.Bool("enabled", s.enabled).Str("indexUrl", s.indexUrl).Bool("customIndex", s.customIndex).Str("fetchedAt", s.fetchedAt);
    o.Bool("offline", s.offline).Int("serial", s.serial).UInt("plugins", s.plugins);
    o.Raw("job", jsonmini::Obj().Str("phase", s.job.phase).Str("id", s.job.id).Str("version", s.job.version)
                     .UInt("bytes", s.job.bytes).UInt("total", s.job.total).Str("message", s.job.message).End());
    o.Str("gate", s.gate).Bool("fetching", s.fetching).Bool("haveIndex", s.haveIndex).Bool("rollback", s.rollback);
    o.Bool("busy", s.busy).Str("error", s.error).Raw("pending", Strings(s.pending)).Raw("notices", Strings(s.notices));
    return o.End();
}

std::string DetailsJson(const Details& d) {
    jsonmini::Obj o;
    ItemFields(o, d.item);
    o.Str("homepage", d.homepage);
    o.Raw("permissions", jsonmini::Obj().Bool("unsafe", d.item.unsafe).Str("filesystem", d.filesystem).End());
    o.Bool("content", d.content).Raw("dependencies", Deps(d.dependencies)).Raw("conflicts", Deps(d.conflicts));
    jsonmini::Arr shots;
    for (const ShotRow& r : d.screenshots)
        shots.Raw(jsonmini::Obj().Int("n", r.n).Str("caption", r.caption).Bool("ready", r.ready).End());
    o.Raw("screenshots", shots.End());
    jsonmini::Arr imports;
    for (const ImportLine& im : d.imports)
        imports.Raw(jsonmini::Obj().Str("title", im.title).Str("publisher", im.publisher).Str("host", im.host).UInt("size", im.size).End());
    o.Raw("imports", imports.End()).Int("importedMaps", d.importedMaps);
    jsonmini::Arr versions;
    for (const VersionRow& v : d.versions)
        versions.Raw(jsonmini::Obj().Str("version", v.version).Str("released", v.released).Str("melange", v.melange)
                         .UInt("size", v.size).Str("changelog", v.changelog).Bool("yanked", v.yanked)
                         .Bool("compatible", v.compatible).End());
    o.Raw("versions", versions.End());
    o.Raw("dependants", Strings(d.dependants)).Raw("conflictsEnabled", Strings(d.conflictsEnabled));
    jsonmini::Arr plan;
    for (const Step& s : d.plan) plan.Raw(jsonmini::Obj().Str("id", s.id).Str("version", s.version).End());
    o.Raw("plan", plan.End()).Str("planError", d.planError);
    return o.End();
}

bool OptFlag(const json::Value& p, const char* key, bool* out, Result& r) {
    const json::Value* m = p.Get(key);
    if (!m || m->IsNull()) return true;
    return rpc::Flag(p, key, out, r);
}

bool Started(const Outcome& o, Result& r) {
    if (o.code) return rpc::Fail(r, o.code, o.message);
    r.json = jsonmini::Obj().Bool("started", true).End();
    return true;
}

bool IdParam(const Call& c, json::Value* p, std::string* id, Result& r) {
    if (!rpc::ParseParams(c, p, r) || !rpc::Str(*p, "id", id, r)) return false;
    if (!spice::ValidModId(*id)) return rpc::Fail(r, rpc::kBadParams, "id is not a plugin id");
    return true;
}

void RpcStatus(const Call&, Result& r, void*) { r.json = StatusJson(GetStatus()); }

void RpcRefresh(const Call&, Result& r, void*) { Started(Refresh(), r); }

void RpcList(const Call& c, Result& r, void*) {
    json::Value p;
    ListQuery q;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "query", &q.query, r, false) || !rpc::Str(p, "category", &q.category, r, false) ||
        !rpc::Str(p, "filter", &q.filter, r, false) || !OptFlag(p, "incompatible", &q.incompatible, r))
        return;
    if (q.filter.empty()) q.filter = "all";
    if (q.filter != "all" && q.filter != "installed" && q.filter != "updates")
        return (void)rpc::Fail(r, rpc::kBadParams, "filter must be all, installed or updates");
    if (q.query.size() > 200) return (void)rpc::Fail(r, rpc::kBadParams, "query is too long");
    jsonmini::Arr a;
    for (const Item& it : List(q)) a.Raw(ItemJson(it));
    r.json = a.End();
}

void RpcDetails(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!IdParam(c, &p, &id, r)) return;
    Details d;
    if (!GetDetails(id, &d, true)) return (void)rpc::Fail(r, rpc::kBadParams, "no plugin '" + id + "' in the list");
    r.json = DetailsJson(d);
}

void RpcInstall(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id, version;
    bool enable = true, replaceManual = false;
    if (!IdParam(c, &p, &id, r) || !rpc::Str(p, "version", &version, r, false) || !OptFlag(p, "enable", &enable, r) ||
        !OptFlag(p, "replaceManual", &replaceManual, r))
        return;
    Started(Install(id, version, enable, replaceManual), r);
}

void RpcUpdate(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (IdParam(c, &p, &id, r)) Started(Update(id), r);
}

void RpcRemove(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    bool deleteData = false;
    if (IdParam(c, &p, &id, r) && OptFlag(p, "deleteData", &deleteData, r)) Started(Remove(id, deleteData), r);
}

void RpcCancel(const Call&, Result& r, void*) { r.json = jsonmini::Obj().Bool("cancelled", Cancel()).End(); }

void RpcHomepage(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id, err;
    if (!IdParam(c, &p, &id, r)) return;
    if (!OpenHomepage(id, &err)) return (void)rpc::Fail(r, rpc::kRefused, err);
    r.json = "{}";
}

bool RouteShots(const oasis::core::Request& rq, oasis::core::Response* out, void*) {
    constexpr size_t kPrefixLen = 13;   // "/store/shots/"
    const std::string rest = std::string(rq.path).substr(kPrefixLen);
    const size_t slash = rest.find('/');
    std::wstring path;
    if (slash != std::string::npos && slash + 2 == rest.size() && rest[slash + 1] >= '1' && rest[slash + 1] <= '6' &&
        spice::ValidModId(rest.substr(0, slash)))
        path = ShotPath(rest.substr(0, slash), rest[slash + 1] - '0');
    if (path.empty()) {
        out->status = 404;
        out->body = "Not Found\n";
        return true;
    }
    out->contentType = path.ends_with(L".png") ? "image/png" : "image/jpeg";
    out->file = path;
    return true;
}

void OnSub(oasis::ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (subscribed) oasis::PublishTo(ch, client, ChannelJson());
}
}  // namespace

void PublishState() {
    if (g_channel && oasis::HasSubscribers(g_channel)) oasis::Publish(g_channel, ChannelJson());
}

void InstallRpc(bool gameOnly) {
    oasis::ChannelOptions o;
    o.overflow = oasis::Overflow::Coalesce;
    g_channel = oasis::AddChannel("store", o);
    oasis::OnSubscribe(g_channel, &OnSub, nullptr);
    const uint32_t kRpcGameOnly = gameOnly ? oasis::kRpcGameOnly : 0u;
    using oasis::kRpcMutating;
    oasis::AddMethod("store.status", &RpcStatus, nullptr, kRpcGameOnly);
    oasis::AddMethod("store.refresh", &RpcRefresh, nullptr, kRpcGameOnly);
    oasis::AddMethod("store.list", &RpcList, nullptr, kRpcGameOnly);
    oasis::AddMethod("store.details", &RpcDetails, nullptr, kRpcGameOnly);
    oasis::AddMethod("store.install", &RpcInstall, nullptr, kRpcMutating | kRpcGameOnly);
    oasis::AddMethod("store.update", &RpcUpdate, nullptr, kRpcMutating | kRpcGameOnly);
    oasis::AddMethod("store.remove", &RpcRemove, nullptr, kRpcMutating | kRpcGameOnly);
    oasis::AddMethod("store.cancel", &RpcCancel, nullptr, kRpcMutating | kRpcGameOnly);
    oasis::AddMethod("store.openHomepage", &RpcHomepage, nullptr, kRpcGameOnly);
    oasis::core::AddRoute("/store/shots/", &RouteShots, nullptr);
}
}  // namespace melange::store
