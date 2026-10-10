// Meshes: mod 3D mesh banks through the engine's graphical resource manager. Owns the [Meshes] ini
// section, the test verbs and the once-per-launch load of enabled content mods' "meshes" banks; the loader itself is
// assets/meshbank.cpp. Nothing here hooks or patches the game.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/meshbank.h"
#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "levels/engine.h"
#include "melange/jlog.h"
#include "melange/testcmd.h"
#include "mods/thumper_internal.h"
#include "weapons/engine.h"

namespace {
namespace mb = melange::assets::meshes;

bool g_enabled = false;
bool g_loadedMods = false;  // the mods' banks were handled this launch (success or not: a section is burnt by one attempt)
int g_frontendFrames = 0;
constexpr int kSettleFrames = 30;  // the same settle the Schemes and Music modules wait at the frontend

// Loads every session-active content mod's "meshes" banks once per launch, in load order. A failing bank is logged
// and skipped; the mod stays enabled. Content-mod changes need a restart anyway, so a mod disabled later keeps its
// banks (and sections) for the rest of the session.
void LoadModBanks() {
    g_loadedMods = true;
    std::vector<melange::thumper::Entry> mods;
    for (auto& e : melange::thumper::Snapshot())
        if (e.sessionActive && e.manifest.content && !e.manifest.meshes.empty()) mods.push_back(std::move(e));
    if (mods.empty()) return;
    std::stable_sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
    unsigned banks = 0, meshes = 0, failed = 0;
    std::string used;
    for (const auto& e : mods) {
        for (const auto& f : e.manifest.meshes) {
            const std::wstring abs = (std::filesystem::path(e.dir) / melange::game::Widen(f.file)).wstring();
            std::string err;
            uint16_t section = 0;
            const uint32_t before = mb::Registered();
            const bool ok = mb::LoadModBank(abs, e.manifest.id, &err, mb::kSceneBinWeapons, &section, true);
            melange::jlog::Rec("meshes", ok ? melange::jlog::Level::Info : melange::jlog::Level::Error, "mod_bank")
                .Str("mod", e.manifest.id).Str("file", f.file).Bool("ok", ok).Uint("section", ok ? section : 0).Str("error", err);
            if (!ok) {
                LOG_ERROR("[meshes] %s/%s: %s", e.manifest.id.c_str(), f.file.c_str(), err.c_str());
                ++failed;
                continue;
            }
            ++banks;
            meshes += mb::Registered() - before;
            used += (used.empty() ? "" : ", ") + std::to_string(section);
        }
    }
    const std::string tail = failed ? ", " + std::to_string(failed) + " failed" : "";
    LOG_INFO("[meshes] %u bank(s) loaded from %zu mod(s), %u mesh(es), sections %s%s", banks, mods.size(), meshes,
             used.empty() ? "none" : used.c_str(), tail.c_str());
}

// The earliest safe main-thread moment: the frontend has been up for a settle period (Thumper resolved the mod list
// at startup, the GRM exists from app init) and the engine answers. Not repeated.
void OnFrame() {
    if (g_loadedMods) return;
    g_frontendFrames = melange::levels::engine::AtFrontend() ? g_frontendFrames + 1 : 0;
    if (g_frontendFrames < kSettleFrames || !melange::weapons::engine::AppReady() || !mb::Available()) return;
    LoadModBanks();
}

std::string Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return std::string(s);
}

// "<path> [modId] [sceneBin]": a path with spaces must be quoted. modId defaults to the file name's first
// dot-separated part ("kindjal.NailBat.xom" -> "kindjal"), which is the naming rule's prefix anyway.
bool VerbLoad(std::string_view a, void*) {
    std::string s = Trim(a);
    std::string path, rest;
    if (!s.empty() && s[0] == '"') {
        const size_t q = s.find('"', 1);
        if (q == std::string::npos) return false;
        path = s.substr(1, q - 1);
        rest = Trim(s.substr(q + 1));
    } else {
        const size_t sp = s.find(' ');
        path = s.substr(0, sp);
        rest = sp == std::string::npos ? "" : Trim(s.substr(sp + 1));
    }
    if (path.empty()) {
        LOG_INFO("[meshes] usage: mesh.load <bank.xom> [modId] [sceneBin]");
        return false;
    }
    std::string modId, binText;
    const size_t sp = rest.find(' ');
    modId = rest.substr(0, sp);
    if (sp != std::string::npos) binText = Trim(rest.substr(sp + 1));
    if (modId.empty()) {
        const size_t slash = path.find_last_of("/\\");
        const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
        modId = file.substr(0, file.find('.'));
    }
    uint8_t bin = mb::kSceneBinWeapons;
    if (!binText.empty()) {
        const long v = std::strtol(binText.c_str(), nullptr, 10);
        if (v < 0 || v > 0x57) {
            LOG_ERROR("[meshes] scene bin %s is not 0..87", binText.c_str());
            return false;
        }
        bin = static_cast<uint8_t>(v);
    }
    std::string err;
    const bool ok = mb::LoadModBank(melange::game::Widen(path), modId, &err, bin);
    melange::jlog::Rec("meshes", ok ? melange::jlog::Level::Info : melange::jlog::Level::Error, "load")
        .Str("path", path).Str("mod", modId).Bool("ok", ok).Str("error", err);
    LOG_INFO("[meshes] load %s (%s): %s%s%s", path.c_str(), modId.c_str(), ok ? "ok" : "refused", err.empty() ? "" : ": ",
             err.c_str());
    return ok;
}

bool VerbResolve(std::string_view a, void*) {
    const std::string name = Trim(a);
    if (name.empty()) return false;
    mb::Info i;
    const bool found = mb::Describe(name.c_str(), &i);
    if (found)
        LOG_INFO("[meshes] resolve %s: descriptor %08x section %u bin %u loaded %d graphSet %08x source '%s'", name.c_str(),
                 static_cast<unsigned>(i.descriptor), i.section, i.sceneBin, i.loaded, static_cast<unsigned>(i.graphSet),
                 i.sourceFile.c_str());
    else
        LOG_INFO("[meshes] resolve %s: not a graphical resource%s", name.c_str(), mb::Available() ? "" : " (GRM unavailable)");
    melange::jlog::Rec("meshes", melange::jlog::Level::Info, "resolve")
        .Str("name", name).Bool("found", found).Bool("loaded", found && i.loaded).Uint("section", found ? i.section : 0);
    return found;
}

bool VerbState(std::string_view, void*) {
    LOG_INFO("[meshes] state: enabled=%d available=%d banks=%u stubs=%u sections=%u..%u", g_enabled, mb::Available(), mb::Count(),
             mb::Registered(), mb::kSectionMin, mb::kSectionMax);
    return true;
}

class Meshes final : public melange::Module {
public:
    const char* Name() const override { return "Meshes"; }
    const char* Description() const override { return "mod 3D mesh banks through the engine's graphical resource manager"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 58; }
    bool Install() override {
        g_enabled = true;
        melange::testcmd::Register("mesh.load", &VerbLoad);
        melange::testcmd::Register("mesh.resolve", &VerbResolve);
        melange::testcmd::Register("mesh.state", &VerbState);
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        // The GRM exists only once the app is up, so availability is reported, not required, here.
        const bool avail = mb::Available();
        melange::jlog::Rec("meshes", melange::jlog::Level::Info, "installed").Bool("available", avail);
        LOG_INFO("[meshes] installed: sites %s; verbs mesh.load / mesh.resolve / mesh.state", avail ? "ok" : "not verified yet");
        return true;
    }
};
}  // namespace

MELANGE_MODULE(Meshes);
