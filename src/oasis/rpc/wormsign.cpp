// The Oasis wormsign.* channels and methods, and the /replays/ route. Every call goes straight through the
// frozen melange/wormsign.h API, so this reports an empty library and refuses to arm until the recorder and
// player land, the same as the overlay panel would.
#include "oasis/providers.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "melange/oasis.h"
#include "melange/wormsign.h"
#include "oasis/core/http.h"
#include "oasis/core/server.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::oasis::providers {
namespace {
namespace ws = melange::wormsign;
using rpc::Fail;

ChannelId g_chTick = 0, g_chDivergence = 0, g_chPlay = 0, g_chLibrary = 0;

std::wstring ReplaysDir() {
    PWSTR p = nullptr;
    std::wstring docs = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) docs = p;
    if (p) CoTaskMemFree(p);
    return docs + L"\\Melange\\replays";
}

// "<name>.wsr" (a recording) or "<name>.zip" (a desync bundle); flat, no traversal.
bool ValidReplayName(std::string_view name) {
    if (name.size() < 5 || name.size() > 200) return false;
    const std::string_view ext = name.substr(name.size() - 4);
    if (ext != ".wsr" && ext != ".zip") return false;
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) return false;
    return name.find("..") == std::string_view::npos;
}

std::string BaseName(const wchar_t* path) {
    const std::wstring w(path);
    const size_t i = w.find_last_of(L"\\/");
    return game::Narrow(i == std::wstring::npos ? w : w.substr(i + 1));
}

std::string Hex64(uint64_t v) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

std::string InfoJson(const ws::ReplayInfo& e) {
    jsonmini::Obj o;
    o.Str("name", BaseName(e.path));
    o.UInt("bytes", e.bytes).UInt("ticks", e.ticks).UInt("inputs", e.inputs).UInt("remoteInputs", e.remoteInputs);
    o.Int("startUnix", e.startUnix).Str("exeBuild", e.exeBuild).Str("melange", e.melange).Str("land", e.land);
    o.Str("contentHash", e.contentHash16);
    o.Bool("online", e.online).Bool("complete", e.complete).Bool("pinned", e.pinned).Bool("flagged", e.flagged);
    return o.End();
}

// Library() follows the same "count then fill" shape as mods::List (oasis/rpc/mods.cpp).
std::vector<ws::ReplayInfo> ReadLibrary() {
    std::vector<ws::ReplayInfo> all(static_cast<size_t>((std::max)(0, ws::Library(nullptr, 0))));
    const int n = all.empty() ? 0 : ws::Library(all.data(), static_cast<int>(all.size()));
    all.resize(static_cast<size_t>((std::max)(0, n)));
    return all;
}

std::string LibraryJson() {
    jsonmini::Arr a;
    for (const auto& e : ReadLibrary()) a.Raw(InfoJson(e));
    return a.End();
}

const char* StateName(ws::PlayState s) {
    switch (s) {
        case ws::PlayState::Idle: return "idle";
        case ws::PlayState::Armed: return "armed";
        case ws::PlayState::Loading: return "loading";
        case ws::PlayState::Playing: return "playing";
        case ws::PlayState::Paused: return "paused";
        case ws::PlayState::Finished: return "finished";
        case ws::PlayState::Diverged: return "diverged";
        case ws::PlayState::Failed: return "failed";
    }
    return "idle";
}

std::string StatusJson(const ws::PlayStatus& s) {
    return jsonmini::Obj()
        .Str("state", StateName(s.state))
        .UInt("tick", s.tick)
        .UInt("ticks", s.ticks)
        .UInt("compared", s.compared)
        .UInt("matched", s.matched)
        .Float("speed", s.speed)
        .Str("error", s.error)
        .End();
}

std::string DivergenceJson(const ws::Divergence& d) {
    jsonmini::Obj o;
    o.Str("source", d.source == ws::Source::Peer ? "peer" : "replay");
    o.UInt("serial", d.serial).UInt("tick", d.tick);
    o.Str("oursEngine", Hex64(d.oursEngine)).Str("theirsEngine", Hex64(d.theirsEngine));
    o.Str("oursMods", Hex64(d.oursMods)).Str("theirsMods", Hex64(d.theirsMods));
    o.Int("compMask", d.compMask).Str("contrib", d.contrib).Str("peer", Hex64(d.peer));
    o.Str("bundle", d.bundle[0] ? BaseName(d.bundle) : "");
    return o.End();
}

// ---------------------------------------------------------------- wormsign.tick: per-client rate (1-10 Hz)
struct TickSub { uint32_t hz = 5; uint64_t lastMs = 0; };
std::unordered_map<int, TickSub> g_tickSubs;  // main thread only (mainThreadSubscribe + the Frame poll)

uint32_t ParseHz(std::string_view filterJson) {
    json::Value v;
    json::Error e;
    uint32_t hz = 5;
    if (json::Parse(filterJson.empty() ? std::string_view("{}") : filterJson, &v, &e) && v.IsObject())
        if (const auto* m = v.Get("hz"); m && m->IsNumber()) hz = static_cast<uint32_t>(m->number);
    return (std::min)(10u, (std::max)(1u, hz));
}

void OnTickSub(ChannelId, int client, std::string_view filterJson, bool subscribed, void*) {
    if (subscribed) g_tickSubs[client] = TickSub{ParseHz(filterJson), 0};
    else g_tickSubs.erase(client);
}

std::string TickPayload(const ws::TickHash& h) {
    jsonmini::Obj o;
    o.UInt("tick", h.tick).Str("engine", Hex64(h.engine)).Str("mods", Hex64(h.mods));
    jsonmini::Arr c;
    for (uint64_t v : h.c) c.Str(Hex64(v));
    o.Raw("c", c.End()).UInt("fpucw", h.fpucw);
    o.Raw("peers", "[]");  // filled in once D's hash exchange reports peers
    return o.End();
}

void PollTick() {
    if (!HasSubscribers(g_chTick) || g_tickSubs.empty()) return;
    ws::TickHash h{};
    if (!ws::InMatch() || !ws::LastTick(&h)) return;
    const uint64_t now = events::LastFrameTick();
    std::string payload;
    for (auto& [client, sub] : g_tickSubs) {
        if (now - sub.lastMs < 1000 / sub.hz) continue;
        if (payload.empty()) payload = TickPayload(h);
        sub.lastMs = now;
        PublishTo(g_chTick, client, payload);
    }
}

// ---------------------------------------------------------------- wormsign.play: on change, 5 Hz while playing
std::string g_lastPlayJson;
uint64_t g_lastPlayMs = 0;

void OnPlaySub(ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (subscribed) PublishTo(ch, client, StatusJson(ws::Status()));
}

void PollPlay() {
    if (!HasSubscribers(g_chPlay)) return;
    const ws::PlayStatus s = ws::Status();
    const std::string j = StatusJson(s);
    const uint64_t now = events::LastFrameTick();
    const bool due = s.state == ws::PlayState::Playing && now - g_lastPlayMs >= 200;
    if (j == g_lastPlayJson && !due) return;
    g_lastPlayJson = j;
    g_lastPlayMs = now;
    Publish(g_chPlay, j);
}

// ---------------------------------------------------------------- wormsign.library: on change, throttled
std::string g_lastLibraryJson;
uint64_t g_lastLibraryMs = 0;

void OnLibrarySub(ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (subscribed) PublishTo(ch, client, g_lastLibraryJson.empty() ? LibraryJson() : g_lastLibraryJson);
}

void PollLibrary(bool force) {
    if (!HasSubscribers(g_chLibrary)) return;
    const uint64_t now = events::LastFrameTick();
    if (!force && now - g_lastLibraryMs < 1000) return;
    g_lastLibraryMs = now;
    const std::string j = LibraryJson();
    if (j == g_lastLibraryJson) return;
    g_lastLibraryJson = j;
    Publish(g_chLibrary, j);
}

void OnFrame() {
    PollTick();
    PollPlay();
    PollLibrary(false);
}

// ---------------------------------------------------------------- divergence tracking (for wormsign.bundle)
std::mutex g_divMx;
std::vector<ws::Divergence> g_divergences;  // most recent first, capped
constexpr size_t kMaxDivergences = 64;

void OnDivergenceCb(const ws::Divergence& d, void*) {
    {
        std::lock_guard lk(g_divMx);
        g_divergences.insert(g_divergences.begin(), d);
        if (g_divergences.size() > kMaxDivergences) g_divergences.resize(kMaxDivergences);
    }
    if (g_chDivergence) Publish(g_chDivergence, DivergenceJson(d));
    g_lastLibraryMs = 0;  // a flagged file may have changed; let the next poll pick it up right away
}

// ---------------------------------------------------------------- methods
void LibraryRpc(const Call&, Result& r, void*) { r.json = LibraryJson(); }

void InfoRpc(const Call& c, Result& r, void*) {
    json::Value p;
    std::string name;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "name", &name, r)) return;
    for (const auto& e : ReadLibrary())
        if (BaseName(e.path) == name) {
            r.json = InfoJson(e);
            return;
        }
    Fail(r, rpc::kBadParams, "no replay '" + name + "'");
}

void PinRpc(const Call& c, Result& r, void*) {
    json::Value p;
    std::string name;
    bool on = false;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "name", &name, r) || !rpc::Flag(p, "on", &on, r)) return;
    if (!ValidReplayName(name)) return (void)Fail(r, rpc::kBadParams, "bad name");
    const std::wstring full = ReplaysDir() + L"\\" + game::Widen(name);
    if (!ws::Pin(full.c_str(), on)) return (void)Fail(r, rpc::kRefused, "no such replay, or pinning is not available yet");
    PollLibrary(true);
    r.json = "true";
}

void BundleRpc(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    const json::Value* sv = p.Get("serial");
    const bool haveSerial = sv && sv->IsNumber();
    const uint32_t serial = haveSerial ? static_cast<uint32_t>(sv->number) : 0;
    std::lock_guard lk(g_divMx);
    for (const auto& d : g_divergences) {
        if (haveSerial && d.serial != serial) continue;
        if (!d.bundle[0]) return (void)Fail(r, rpc::kRefused, "the bundle has not been written yet");
        r.json = jsonmini::Obj().Str("bundle", BaseName(d.bundle)).End();
        return;
    }
    Fail(r, rpc::kRefused, "no divergence recorded for that match (the desync detector is not installed yet)");
}

void ArmRpc(const Call& c, Result& r, void*) {
    json::Value p;
    std::string name;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "name", &name, r)) return;
    if (name.size() < 5 || name.substr(name.size() - 4) != ".wsr" || !ValidReplayName(name))
        return (void)Fail(r, rpc::kBadParams, "name must be a .wsr file");
    const std::wstring full = ReplaysDir() + L"\\" + game::Widen(name);
    char err[128] = {};
    if (!ws::Arm(full.c_str(), err, sizeof err)) return (void)Fail(r, rpc::kRefused, err[0] ? err : "could not arm");
    PollPlay();
    r.json = StatusJson(ws::Status());
}

void ControlRpc(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    if (const auto* m = p.Get("disarm"); m && m->IsBool() && m->boolean) ws::Disarm();
    if (const auto* m = p.Get("pause"); m && m->IsBool()) ws::SetPaused(m->boolean);
    if (const auto* m = p.Get("speed"); m && m->IsNumber()) ws::SetSpeed(static_cast<float>(m->number));
    if (const auto* m = p.Get("runTo"); m && m->IsNumber() && m->number >= 0) ws::RunTo(static_cast<uint32_t>(m->number));
    PollPlay();
    r.json = StatusJson(ws::Status());
}

void DetailRpc(const Call&, Result& r, void*) {
    // src/wormsign/detail.h does not exist in this build yet; wire its Get(tick, &rec) in here once it lands.
    Fail(r, rpc::kRefused, "detail records are not available in this build yet");
}

bool RouteReplays(const core::Request& rq, core::Response* out, void*) {
    constexpr size_t kPrefixLen = 10;  // "/replays/"
    const std::string name = rq.path.substr(kPrefixLen);
    if (!ValidReplayName(name)) {
        out->status = 404;
        out->body = "Not Found\n";
        return true;
    }
    const std::wstring full = ReplaysDir() + L"\\" + game::Widen(name);
    const DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        out->status = 404;
        out->body = "Not Found\n";
        return true;
    }
    out->contentType = name.substr(name.size() - 4) == ".zip" ? "application/zip" : "application/octet-stream";
    out->file = full;
    return true;
}
}  // namespace

void InstallWormsign() {
    ChannelOptions tickOpt;
    tickOpt.overflow = Overflow::Coalesce;
    tickOpt.mainThreadSubscribe = true;
    g_chTick = AddChannel("wormsign.tick", tickOpt);
    if (g_chTick) OnSubscribe(g_chTick, &OnTickSub, nullptr);

    ChannelOptions divOpt;
    divOpt.overflow = Overflow::DropOldest;
    g_chDivergence = AddChannel("wormsign.divergence", divOpt);

    ChannelOptions playOpt;
    playOpt.overflow = Overflow::Coalesce;
    playOpt.mainThreadSubscribe = true;
    g_chPlay = AddChannel("wormsign.play", playOpt);
    if (g_chPlay) OnSubscribe(g_chPlay, &OnPlaySub, nullptr);

    ChannelOptions libOpt;
    libOpt.overflow = Overflow::Coalesce;
    libOpt.mainThreadSubscribe = true;
    g_chLibrary = AddChannel("wormsign.library", libOpt);
    if (g_chLibrary) OnSubscribe(g_chLibrary, &OnLibrarySub, nullptr);

    AddMethod("wormsign.library", &LibraryRpc, nullptr);
    AddMethod("wormsign.info", &InfoRpc, nullptr);
    AddMethod("wormsign.pin", &PinRpc, nullptr, kRpcMutating);
    AddMethod("wormsign.bundle", &BundleRpc, nullptr, kRpcMutating);
    AddMethod("wormsign.arm", &ArmRpc, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("wormsign.control", &ControlRpc, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("wormsign.detail", &DetailRpc, nullptr, kRpcGameOnly);
    core::AddRoute("/replays/", &RouteReplays, nullptr);

    ws::OnDivergence(&OnDivergenceCb, nullptr);
    events::Subscribe(events::Event::Frame, &OnFrame);
}
}  // namespace melange::oasis::providers
