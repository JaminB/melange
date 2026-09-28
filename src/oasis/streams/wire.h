#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "melange/jlog.h"
#include "melange/mods.h"
#include "melange/oasis.h"
#include "melange/render.h"

// Pure logic for the streams channels (log, net, bus, bus.counts, lobby, stats): filter parsing/matching and
// wire payload building. No bus::, no jlog::Tail, no Steam, no game dependency: everything here is a function
// of its arguments, so tests/oasis_streams_selftest.cpp drives it directly. streams/*.cpp supply the live data.
namespace melange::oasis::streams {

// ---- log / net: filter {minLevel, cats, text} (docs/m3-design.md §3.5)
struct LogFilter {
    jlog::Level minLevel = jlog::Level::Trace;
    std::vector<std::string> cats;  // empty: every category
    std::string text;               // empty: no substring filter; matched against the raw jlog line
};
LogFilter ParseLogFilter(std::string_view filterJson);
bool MatchesLog(const LogFilter& f, const jlog::Line& l);
// {seq, lvl, cat, ts, j}: seq/ts are jlog's own (not the wire envelope's per-client seq); j is the raw jlog line.
std::string BuildLogPayload(const jlog::Line& l);

// ---- bus: filter {names (required), path, decode}
struct BusFilter {
    std::vector<std::string> names;  // exact name, or "Prefix.*"; empty matches nothing (names is required)
    std::string path;                // "" (any), "post" or "deliver"
    bool decode = false;
};
BusFilter ParseBusFilter(std::string_view filterJson);
bool IsPrefixPattern(std::string_view pattern);            // "Foo.*" (at least one character before ".*")
std::string_view PrefixOf(std::string_view pattern);       // "Foo." from "Foo.*"
bool NameMatches(std::string_view name, std::string_view pattern);
bool AnyNameMatches(std::string_view name, const std::vector<std::string>& patterns);
// {seq, frame, name, cls, path, handle, d?}. `decodedJson` is a complete JSON object (as bus::Decode's fields,
// wrapped in braces) or empty to omit `d` (no decoder, or no client asked for one).
std::string BuildBusPayload(uint64_t seq, uint64_t frame, std::string_view name, std::string_view cls,
                             std::string_view path, int32_t handle, std::string_view decodedJson);

// ---- lobby: {inLobby, local, peers}
std::string BuildLobbyPayload(bool inLobby, const mods::ContentId& local, const mods::Peer* peers, int n);

// ---- stats: server counters + render timing, 1 Hz
std::string BuildStatsPayload(const Stats& s, const render::Timing& t);

}  // namespace melange::oasis::streams
