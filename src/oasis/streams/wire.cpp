#include "oasis/streams/wire.h"

#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::oasis::streams {
namespace {

const char* PeerStatusName(mods::PeerStatus s) {
    switch (s) {
        case mods::PeerStatus::Unknown: return "unknown";
        case mods::PeerStatus::Vanilla: return "vanilla";
        case mods::PeerStatus::MelangeVanilla: return "melangeVanilla";
        case mods::PeerStatus::Match: return "match";
        case mods::PeerStatus::Mismatch: return "mismatch";
    }
    return "?";
}

jlog::Level ParseLevelName(std::string_view s, jlog::Level def) {
    static constexpr std::pair<const char*, jlog::Level> kLevels[] = {
        {"trace", jlog::Level::Trace}, {"debug", jlog::Level::Debug}, {"info", jlog::Level::Info},
        {"warn", jlog::Level::Warn},   {"error", jlog::Level::Error}, {"fatal", jlog::Level::Fatal},
    };
    for (auto& [name, lvl] : kLevels)
        if (s == name) return lvl;
    return def;
}

}  // namespace

LogFilter ParseLogFilter(std::string_view filterJson) {
    LogFilter f;
    json::Value v;
    json::Error e;
    if (!json::Parse(filterJson, &v, &e) || !v.IsObject()) return f;
    if (const json::Value* lv = v.Get("minLevel"); lv && lv->IsString()) f.minLevel = ParseLevelName(lv->string, f.minLevel);
    if (const json::Value* cv = v.Get("cats"); cv && cv->IsArray())
        for (const auto& item : cv->items)
            if (item.IsString()) f.cats.push_back(item.string);
    if (const json::Value* tv = v.Get("text"); tv && tv->IsString()) f.text = tv->string;
    return f;
}

bool MatchesLog(const LogFilter& f, const jlog::Line& l) {
    if (static_cast<uint8_t>(l.lvl) < static_cast<uint8_t>(f.minLevel)) return false;
    if (!f.cats.empty()) {
        bool found = false;
        for (const auto& c : f.cats) found |= (c == l.category);
        if (!found) return false;
    }
    if (!f.text.empty() && l.json.find(f.text) == std::string::npos) return false;
    return true;
}

std::string BuildLogPayload(const jlog::Line& l) {
    return jsonmini::Obj()
        .UInt("seq", l.seq)
        .Str("lvl", jlog::LevelName(l.lvl))
        .Str("cat", l.category)
        .Float("ts", l.t)
        .Raw("j", l.json)
        .End();
}

BusFilter ParseBusFilter(std::string_view filterJson) {
    BusFilter f;
    json::Value v;
    json::Error e;
    if (!json::Parse(filterJson, &v, &e) || !v.IsObject()) return f;
    if (const json::Value* nv = v.Get("names"); nv && nv->IsArray())
        for (const auto& item : nv->items)
            if (item.IsString()) f.names.push_back(item.string);
    if (const json::Value* pv = v.Get("path"); pv && pv->IsString()) f.path = pv->string;
    if (const json::Value* dv = v.Get("decode"); dv && dv->IsBool()) f.decode = dv->boolean;
    return f;
}

bool IsPrefixPattern(std::string_view pattern) { return pattern.size() >= 3 && pattern.ends_with(".*"); }

std::string_view PrefixOf(std::string_view pattern) {
    return IsPrefixPattern(pattern) ? pattern.substr(0, pattern.size() - 1) : pattern;  // "Foo." from "Foo.*"
}

bool NameMatches(std::string_view name, std::string_view pattern) {
    if (IsPrefixPattern(pattern)) return name.starts_with(PrefixOf(pattern));
    return name == pattern;
}

bool AnyNameMatches(std::string_view name, const std::vector<std::string>& patterns) {
    for (const auto& p : patterns)
        if (NameMatches(name, p)) return true;
    return false;
}

std::string BuildBusPayload(uint64_t seq, uint64_t frame, std::string_view name, std::string_view cls,
                             std::string_view path, int32_t handle, std::string_view decodedJson) {
    jsonmini::Obj o;
    o.UInt("seq", seq).UInt("frame", frame).Str("name", name).Str("cls", cls).Str("path", path).Int("handle", handle);
    if (!decodedJson.empty()) o.Raw("d", decodedJson);
    return o.End();
}

std::string BuildLobbyPayload(bool inLobby, const mods::ContentId& local, const mods::Peer* peers, int n) {
    jsonmini::Arr pa;
    for (int i = 0; i < n; ++i) {
        const mods::Peer& p = peers[i];
        char steamId[24];
        snprintf(steamId, sizeof(steamId), "%llu", static_cast<unsigned long long>(p.steamId));
        pa.Raw(jsonmini::Obj()
                   .Str("steamId", steamId)
                   .Str("name", p.name)
                   .Str("status", PeerStatusName(p.status))
                   .Str("hash16", p.hash16)
                   .Str("version", p.version)
                   .End());
    }
    return jsonmini::Obj()
        .Bool("inLobby", inLobby)
        .Raw("local", jsonmini::Obj()
                           .Str("hash", local.hash)
                           .UInt("contentMods", local.contentMods)
                           .UInt("modMessages", local.modMessages)
                           .Bool("vanilla", local.vanilla)
                           .End())
        .Raw("peers", pa.End())
        .End();
}

std::string BuildStatsPayload(const Stats& s, const render::Timing& t) {
    return jsonmini::Obj()
        .UInt("clients", s.clients)
        .UInt("channels", s.channels)
        .UInt("methods", s.methods)
        .UInt("framesOut", s.framesOut)
        .UInt("bytesOut", s.bytesOut)
        .UInt("bytesIn", s.bytesIn)
        .UInt("dropped", s.dropped)
        .UInt("authFailures", s.authFailures)
        .UInt("rpcCalls", s.rpcCalls)
        .Float("busyMsP50", t.busyMsP50)
        .Float("busyMsP95", t.busyMsP95)
        .Float("fps", t.fps)
        .UInt("frames", t.frames)
        .End();
}

}  // namespace melange::oasis::streams
