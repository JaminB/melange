// Offline self-test for Component C's pure wire logic (src/oasis/streams/wire.cpp): filter parsing and
// matching for log/net and bus, bus name-pattern matching, and the JSON payloads shipped on each channel.
// No game, no bus::, no Steam, no jlog writer: everything here is driven directly with synthetic values.
#include <cstdio>
#include <string>

#include "melange/jlog.h"
#include "melange/mods.h"
#include "melange/oasis.h"
#include "melange/render.h"
#include "oasis/streams/wire.h"
#include "tools/json_read.h"

namespace streams = melange::oasis::streams;
namespace jlog = melange::jlog;
namespace mods = melange::mods;

namespace {
int g_pass = 0, g_fail = 0;

void Expect(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
        return;
    }
    ++g_fail;
    printf("FAIL: %s\n", what);
}

jlog::Line MakeLine(uint64_t seq, jlog::Level lvl, std::string cat, std::string msg, double t = 0) {
    std::string json = "{\"v\":1,\"seq\":" + std::to_string(seq) + ",\"lvl\":\"" + jlog::LevelName(lvl) + "\",\"cat\":\"" +
                        cat + "\",\"msg\":\"" + msg + "\"}";
    return jlog::Line{seq, lvl, std::move(cat), std::move(json), t};
}

void TestLogFilterParsing() {
    streams::LogFilter def = streams::ParseLogFilter("{}");
    Expect(def.minLevel == jlog::Level::Trace, "log filter: default minLevel is Trace (no floor)");
    Expect(def.cats.empty(), "log filter: default cats is empty (every category)");
    Expect(def.text.empty(), "log filter: default text is empty (no substring filter)");

    streams::LogFilter garbage = streams::ParseLogFilter("not json");
    Expect(garbage.minLevel == jlog::Level::Trace && garbage.cats.empty(), "log filter: malformed JSON falls back to defaults");

    streams::LogFilter f = streams::ParseLogFilter(R"({"minLevel":"warn","cats":["test","net"],"text":"boom"})");
    Expect(f.minLevel == jlog::Level::Warn, "log filter: minLevel parsed");
    Expect(f.cats.size() == 2 && f.cats[0] == "test" && f.cats[1] == "net", "log filter: cats parsed in order");
    Expect(f.text == "boom", "log filter: text parsed");
}

void TestLogFilterMatching() {
    streams::LogFilter f;
    f.minLevel = jlog::Level::Warn;
    jlog::Line info = MakeLine(1, jlog::Level::Info, "test", "hi");
    jlog::Line warn = MakeLine(2, jlog::Level::Warn, "test", "hi");
    Expect(!streams::MatchesLog(f, info), "log match: below minLevel is excluded");
    Expect(streams::MatchesLog(f, warn), "log match: at minLevel is included");

    f = {};
    f.cats = {"net", "handshake"};
    Expect(streams::MatchesLog(f, MakeLine(1, jlog::Level::Info, "net", "x")), "log match: category in the list");
    Expect(!streams::MatchesLog(f, MakeLine(1, jlog::Level::Info, "shader", "x")), "log match: category not in the list is excluded");

    f = {};
    f.text = "needle";
    Expect(streams::MatchesLog(f, MakeLine(1, jlog::Level::Info, "test", "a needle in it")), "log match: substring found");
    Expect(!streams::MatchesLog(f, MakeLine(1, jlog::Level::Info, "test", "nothing here")), "log match: substring absent is excluded");
}

void TestLogPayload() {
    jlog::Line l = MakeLine(42, jlog::Level::Error, "test", "boom", 12.5);
    std::string payload = streams::BuildLogPayload(l);
    melange::json::Value v;
    melange::json::Error e;
    Expect(melange::json::Parse(payload, &v, &e) && v.IsObject(), "log payload: valid JSON object");
    if (v.IsObject()) {
        Expect(v.Get("seq") && v.Get("seq")->number == 42, "log payload: seq");
        Expect(v.Get("lvl") && v.Get("lvl")->string == "error", "log payload: lvl");
        Expect(v.Get("cat") && v.Get("cat")->string == "test", "log payload: cat");
        Expect(v.Get("ts") && v.Get("ts")->number == 12.5, "log payload: ts");
        Expect(v.Get("j") && v.Get("j")->IsObject(), "log payload: j is the embedded jlog line, still an object");
    }
}

void TestBusNamePatterns() {
    Expect(!streams::IsPrefixPattern("GameLogic.Turn.Started"), "bus pattern: an exact name is not a prefix pattern");
    Expect(streams::IsPrefixPattern("GameLogic.Turn.*"), "bus pattern: Foo.* is a prefix pattern");
    Expect(!streams::IsPrefixPattern(".*"), "bus pattern: bare .* (nothing before the dot) is not a prefix pattern");
    Expect(streams::PrefixOf("GameLogic.Turn.*") == "GameLogic.Turn.", "bus pattern: PrefixOf keeps the trailing dot");

    Expect(streams::NameMatches("GameLogic.Turn.Started", "GameLogic.Turn.Started"), "bus match: exact name matches itself");
    Expect(!streams::NameMatches("GameLogic.Turn.Started", "GameLogic.Turn.Ended"), "bus match: exact name rejects a different one");
    Expect(streams::NameMatches("GameLogic.Turn.Started", "GameLogic.Turn.*"), "bus match: prefix pattern matches a member");
    Expect(streams::NameMatches("GameLogic.Turn.Started", "GameLogic.*"), "bus match: a shorter prefix still matches");
    Expect(!streams::NameMatches("GameLogic2.Turn.Started", "GameLogic.*"), "bus match: prefix requires the literal dot, not just a shared substring");
    Expect(!streams::NameMatches("GameLogicX", "GameLogic.*"), "bus match: no dot after the prefix does not match");

    Expect(streams::AnyNameMatches("Camera.HasUpdated", {"GameLogic.*", "Camera.HasUpdated"}), "bus match: AnyNameMatches finds the matching pattern");
    Expect(!streams::AnyNameMatches("Camera.HasUpdated", {"GameLogic.*"}), "bus match: AnyNameMatches false when nothing matches");
    Expect(!streams::AnyNameMatches("Camera.HasUpdated", {}), "bus match: an empty pattern list matches nothing");
}

void TestBusFilterParsing() {
    streams::BusFilter f = streams::ParseBusFilter(R"({"names":["GameLogic.Turn.*"],"path":"post","decode":true})");
    Expect(f.names.size() == 1 && f.names[0] == "GameLogic.Turn.*", "bus filter: names parsed");
    Expect(f.path == "post", "bus filter: path parsed");
    Expect(f.decode, "bus filter: decode parsed");

    streams::BusFilter def = streams::ParseBusFilter("{}");
    Expect(def.names.empty() && def.path.empty() && !def.decode, "bus filter: missing members default to empty/false");
}

void TestBusPayload() {
    std::string plain = streams::BuildBusPayload(7, 1000, "GameLogic.Turn.Started", "TurnStartedMessage", "post", -1, "");
    melange::json::Value v;
    melange::json::Error e;
    Expect(melange::json::Parse(plain, &v, &e) && v.IsObject(), "bus payload: valid JSON object");
    if (v.IsObject()) {
        Expect(v.Get("seq") && v.Get("seq")->number == 7, "bus payload: seq");
        Expect(v.Get("frame") && v.Get("frame")->number == 1000, "bus payload: frame");
        Expect(v.Get("name") && v.Get("name")->string == "GameLogic.Turn.Started", "bus payload: name");
        Expect(v.Get("cls") && v.Get("cls")->string == "TurnStartedMessage", "bus payload: cls");
        Expect(v.Get("path") && v.Get("path")->string == "post", "bus payload: path");
        Expect(v.Get("handle") && v.Get("handle")->number == -1, "bus payload: handle");
        Expect(!v.Get("d"), "bus payload: d is absent when nothing was decoded");
    }

    std::string decoded = streams::BuildBusPayload(8, 1001, "X", "Y", "post", 3, R"({"foo":1})");
    Expect(melange::json::Parse(decoded, &v, &e) && v.IsObject(), "bus payload: valid JSON object with d");
    if (v.IsObject()) {
        const melange::json::Value* d = v.Get("d");
        Expect(d && d->IsObject() && d->Get("foo") && d->Get("foo")->number == 1, "bus payload: d carries the decoded fields");
    }
}

void TestLobbyPayload() {
    mods::ContentId local{};
    snprintf(local.hash, sizeof(local.hash), "%s", "abc123");
    local.contentMods = 2;
    local.modMessages = 5;
    local.vanilla = false;

    mods::Peer peers[2]{};
    snprintf(peers[0].name, sizeof(peers[0].name), "%s", "Alice");
    peers[0].status = mods::PeerStatus::Match;
    peers[0].steamId = 76561197960287930ull;
    snprintf(peers[1].name, sizeof(peers[1].name), "%s", "Bob");
    peers[1].status = mods::PeerStatus::Vanilla;

    std::string payload = streams::BuildLobbyPayload(true, local, peers, 2);
    melange::json::Value v;
    melange::json::Error e;
    Expect(melange::json::Parse(payload, &v, &e) && v.IsObject(), "lobby payload: valid JSON object");
    if (v.IsObject()) {
        Expect(v.Get("inLobby") && v.Get("inLobby")->boolean, "lobby payload: inLobby");
        const melange::json::Value* l = v.Get("local");
        Expect(l && l->IsObject() && l->Get("hash") && l->Get("hash")->string == "abc123", "lobby payload: local.hash");
        Expect(l && l->Get("contentMods") && l->Get("contentMods")->number == 2, "lobby payload: local.contentMods");
        const melange::json::Value* p = v.Get("peers");
        Expect(p && p->IsArray() && p->items.size() == 2, "lobby payload: two peers");
        if (p && p->items.size() == 2) {
            Expect(p->items[0].Get("name")->string == "Alice", "lobby payload: peer 0 name");
            Expect(p->items[0].Get("status")->string == "match", "lobby payload: peer 0 status");
            Expect(p->items[1].Get("name")->string == "Bob", "lobby payload: peer 1 name");
            Expect(p->items[1].Get("status")->string == "vanilla", "lobby payload: peer 1 status");
            Expect(p->items[0].Get("steamId")->string == "76561197960287930", "lobby payload: a 64-bit id survives as a string");
        }
    }

    std::string empty = streams::BuildLobbyPayload(false, mods::ContentId{}, nullptr, 0);
    Expect(melange::json::Parse(empty, &v, &e) && v.IsObject() && v.Get("peers") && v.Get("peers")->items.empty(),
           "lobby payload: no peers gives an empty array, not an error");
}

void TestStatsPayload() {
    melange::oasis::Stats s{};
    s.clients = 2;
    s.channels = 7;
    s.methods = 12;
    s.dropped = 3;
    melange::render::Timing t{};
    t.busyMsP50 = 3.4;
    t.busyMsP95 = 3.9;
    t.fps = 280.5;
    std::string payload = streams::BuildStatsPayload(s, t);
    melange::json::Value v;
    melange::json::Error e;
    Expect(melange::json::Parse(payload, &v, &e) && v.IsObject(), "stats payload: valid JSON object");
    if (v.IsObject()) {
        Expect(v.Get("clients") && v.Get("clients")->number == 2, "stats payload: clients");
        Expect(v.Get("dropped") && v.Get("dropped")->number == 3, "stats payload: dropped");
        Expect(v.Get("busyMsP50") && v.Get("busyMsP50")->number == 3.4, "stats payload: busyMsP50");
        Expect(v.Get("fps") && v.Get("fps")->number == 280.5, "stats payload: fps");
    }
}

}  // namespace

int main() {
    TestLogFilterParsing();
    TestLogFilterMatching();
    TestLogPayload();
    TestBusNamePatterns();
    TestBusFilterParsing();
    TestBusPayload();
    TestLobbyPayload();
    TestStatsPayload();

    printf("%d/%d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
