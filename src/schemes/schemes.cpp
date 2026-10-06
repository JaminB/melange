// Schemes: game styles (DATA.LockedSchemes) and team-editor weapon presets (DATA.LockedWeapons) from enabled mods'
// spice.json "schemes" and "factoryWeapons". At the frontend the two resources are rebuilt from the game's own
// LOCAL.XOM with the mods' entries appended (builder.cpp), written under Melange\cache\schemes and loaded over the
// originals (section 0, overwrite); each entry's display text is added as a string resource.
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "levels/engine.h"
#include "melange/jlog.h"
#include "melange/mods.h"
#include "mods/thumper_internal.h"
#include "schemes/builder.h"
#include "weapons/engine.h"
#include "xom/xom.h"

namespace {
namespace fs = std::filesystem;
namespace sc = melange::schemes;
namespace weng = melange::weapons::engine;
namespace leng = melange::levels::engine;

constexpr int kSettleFrames = 30;
constexpr uint32_t kTextSection = 12;

int g_frontendFrames = 0;
bool g_dirty = true;            // the enabled mods changed (or nothing was registered yet)
bool g_loaded = false;          // a bank was loaded this session, so a rebuild must also undo it
// The collectives our banks registered. The game loads LOCAL.XOM and the profile after the frontend first counts as
// ready, which puts its own objects back under the same names, so the frame hook watches for ours to be replaced.
uintptr_t g_ourSchemes = 0, g_ourWeapons = 0;
int g_checkFrames = 0;
std::unique_ptr<melange::xom::Document> g_local;
bool g_localTried = false;

fs::path GameDir() { return fs::path(melange::game::GameDir()); }
fs::path CacheDir() { return GameDir() / L"Melange" / L"cache" / L"schemes"; }

const melange::xom::Document* Local() {
    if (g_local) return g_local.get();
    if (g_localTried) return nullptr;
    g_localTried = true;
    std::ifstream f(GameDir() / L"Data" / L"Tweak" / L"LOCAL.XOM", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto doc = std::make_unique<melange::xom::Document>();
    std::string err;
    if (bytes.empty() || !melange::xom::parse(bytes.data(), bytes.size(), *doc, &err)) {
        LOG_ERROR("[schemes] Data/Tweak/LOCAL.XOM could not be read%s%s", err.empty() ? "" : ": ", err.c_str());
        melange::jlog::Rec("schemes", melange::jlog::Level::Error, "local_unreadable").Str("why", err);
        return nullptr;
    }
    g_local = std::move(doc);
    return g_local.get();
}

bool WriteAtomic(const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

void LogErrors(const sc::Built& b) {
    for (const auto& e : b.errors) {
        LOG_ERROR("[schemes] %s/%s: %s", e.mod.c_str(), e.file.c_str(), e.text.c_str());
        melange::jlog::Rec("schemes", melange::jlog::Level::Error, "entry_refused")
            .Str("mod", e.mod).Str("file", e.file).Str("why", e.text);
    }
}

// Writes and loads one bank, then adds the entries' display texts.
void Load(const char* resource, const char* file, const sc::Built& b) {
    LogErrors(b);
    if (!b.fatal.empty()) {
        LOG_ERROR("[schemes] %s: %s; left as it is", resource, b.fatal.c_str());
        melange::jlog::Rec("schemes", melange::jlog::Level::Error, "bank_failed").Str("resource", resource).Str("why", b.fatal);
        return;
    }
    if (b.bank.empty()) return;
    const fs::path path = CacheDir() / file;
    if (!WriteAtomic(path, b.bank)) {
        LOG_ERROR("[schemes] %s: %s could not be written", resource, melange::game::Narrow(path.wstring()).c_str());
        return;
    }
    const std::string rel = std::string("Melange/cache/schemes/") + file;
    const int rc = weng::LoadBank(rel.c_str(), 0);
    if (rc != 0) {
        LOG_ERROR("[schemes] %s: LoadBank(%s) returned %d; left as it is", resource, rel.c_str(), rc);
        melange::jlog::Rec("schemes", melange::jlog::Level::Error, "bank_failed").Str("resource", resource).Int("rc", rc);
        return;
    }
    g_loaded = true;
    (std::string(resource) == "DATA.LockedSchemes" ? g_ourSchemes : g_ourWeapons) = weng::Lookup(resource);
    std::string mods;
    int added = 0;
    for (const auto& t : b.added) {
        if (!leng::AddString(t.key.c_str(), t.title.c_str(), kTextSection))
            LOG_WARN("[schemes] %s: the text for %s could not be added", t.mod.c_str(), t.key.c_str());
        if (mods.find(t.mod) == std::string::npos) mods += (mods.empty() ? "" : ", ") + t.mod;
        ++added;
    }
    LOG_INFO("[schemes] %s: %d built-in + %d from mods (%s), bank %s", resource, b.builtIn, added, mods.c_str(), rel.c_str());
    melange::jlog::Rec("schemes", melange::jlog::Level::Info, "bank")
        .Str("resource", resource).Uint("builtIn", static_cast<uint64_t>(b.builtIn)).Uint("added", static_cast<uint64_t>(added));
}

// Rebuilds both resources from LOCAL.XOM plus every enabled mod's entries; safe to repeat (LoadBank overwrites).
void Register() {
    g_dirty = false;
    std::vector<sc::Source> schemes, weapons;
    for (const auto& e : melange::thumper::Snapshot()) {
        if (e.state != melange::mods::State::Enabled) continue;
        const auto& m = e.manifest;
        if (!m.schemes.empty()) {
            sc::Source s{m.id, fs::path(e.dir), {}};
            for (const auto& f : m.schemes) s.files.push_back(f.file);
            schemes.push_back(std::move(s));
        }
        if (!m.factoryWeapons.empty()) {
            sc::Source s{m.id, fs::path(e.dir), {}};
            for (const auto& f : m.factoryWeapons) s.files.push_back(f.file);
            weapons.push_back(std::move(s));
        }
    }
    if (schemes.empty() && weapons.empty() && !g_loaded) return;
    const auto t0 = std::chrono::steady_clock::now();
    const melange::xom::Document* local = Local();
    if (!local) return;
    // Once a bank was loaded, a rebuild without entries puts LOCAL.XOM's own lists back.
    if (!schemes.empty() || g_loaded) Load("DATA.LockedSchemes", "LockedSchemes.XOM", sc::BuildSchemes(*local, schemes, g_loaded));
    if (!weapons.empty() || g_loaded) Load("DATA.LockedWeapons", "LockedWeapons.XOM", sc::BuildFactoryWeapons(*local, weapons, g_loaded));
    LOG_INFO("[schemes] registration took %.2f ms",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// True when a resource we registered no longer answers to its name: the game loaded its own copy over it.
bool Replaced(const char* resource, uintptr_t ours) { return ours && weng::Lookup(resource) != ours; }

void OnFrame() {
    const bool front = leng::AtFrontend();
    g_frontendFrames = front ? g_frontendFrames + 1 : 0;
    if (!front || g_frontendFrames < kSettleFrames || !weng::AppReady() || !weng::Drm()) return;
    if (!g_dirty && g_loaded && ++g_checkFrames >= kSettleFrames) {
        g_checkFrames = 0;
        if (Replaced("DATA.LockedSchemes", g_ourSchemes) || Replaced("DATA.LockedWeapons", g_ourWeapons)) {
            LOG_INFO("[schemes] the game reloaded its own lists; registering again");
            g_dirty = true;
        }
    }
    if (g_dirty) Register();
}

class Schemes final : public melange::Module {
public:
    const char* Name() const override { return "Schemes"; }
    const char* Description() const override { return "game styles and team-editor weapon presets from mods"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 57; }
    bool Install() override {
        melange::mods::OnChange([](void*) { g_dirty = true; }, nullptr);
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        LOG_INFO("[schemes] installed");
        return true;
    }
};
}  // namespace

MELANGE_MODULE(Schemes);
