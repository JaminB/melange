// Level scripts: a level's sandboxed sim script, loaded after the sim mods and only when its level loads.
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/events.h"
#include "core/log.h"
#include "erg/luagen.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_core.h"
#include "melange/bus.h"
#include "melange/levels.h"
#include "tools/hash.h"

namespace core = melange::simcore;

namespace {
std::vector<core::LevelSource> g_pack;
std::optional<core::LevelSource> g_test;
std::mutex g_mx;                                     // the Test script arrives on a server thread
std::optional<core::LevelSource> g_pendingTest;
std::atomic<bool> g_testChanged{false};

core::LevelSource ToSource(const melange::simbridge::LevelSim& s) {
    core::LevelSource l;
    l.key = s.key;
    l.stem = s.stem;
    l.sha256 = melange::hashutil::Sha256Hex(s.text.data(), s.text.size());
    l.knots = s.knots;
    l.src.id = s.mod + ":" + s.slug;
    l.src.version = "level";
    l.src.chunkName = s.chunkName;
    l.src.code = s.text;
    return l;
}

void Apply() {
    std::vector<core::LevelSource> all = g_pack;
    if (g_test) all.push_back(*g_test);
    core::SetLevelSources(std::move(all));
}

void OnLevelStart(const melange::levels::LevelStart& s, void*) { core::SetLevel(s.key ? s.key : ""); }

void OnTurnStarted(const melange::bus::MessageView&, void*) { core::TurnStarted(); }

void OnFrame() {
    if (!g_testChanged.exchange(false)) return;
    {
        std::lock_guard lk(g_mx);
        g_test = std::move(g_pendingTest);
        g_pendingTest.reset();
    }
    if (g_test) LOG_INFO("[sim] Test level script for %s (%zu bytes)", g_test->key.c_str(), g_test->src.code.size());
    Apply();
}
}  // namespace

namespace melange::simbridge {
void SetLevelSims(std::vector<LevelSim> sims) {
    g_pack.clear();
    for (const auto& s : sims) {
        g_pack.push_back(ToSource(s));
        LOG_INFO("[sim] level script %s:%s for %s (%zu bytes, %zu knots)", s.mod.c_str(), s.slug.c_str(), s.key.c_str(),
                 s.text.size(), s.knots.size());
    }
    Apply();
}

void SetTestLevelSim(const LevelSim& s) {
    std::lock_guard lk(g_mx);
    g_pendingTest = s.text.empty() ? std::nullopt : std::optional(ToSource(s));
    g_testChanged = true;
}

std::vector<std::pair<std::string, std::string>> LevelKnots(const std::string& stem, const std::string& chunkText) {
    std::vector<std::pair<std::string, std::string>> out;
    erg::luagen::ChunkSpec spec;
    if (chunkText.empty() || !erg::luagen::Parse(stem, chunkText, &spec)) return out;
    if (spec.knots)
        for (int i = 0; i < 8; ++i) out.emplace_back("WORM" + std::to_string(i), "spawn");
    for (const auto& o : spec.objects)
        if (!o.knot.empty()) out.emplace_back(o.knot, erg::ObjectTypeName(o.type));
    return out;
}

std::string LevelSimDigest() { return core::LevelDigest(); }

void InstallLevelSims() {
    levels::OnLevelStart(&OnLevelStart, nullptr);
    events::Subscribe(events::Event::Frame, &OnFrame);
    if (bus::Installed()) bus::SubscribeName("GameLogic.Turn.Started", bus::Path::Post, &OnTurnStarted);
}
}  // namespace melange::simbridge
