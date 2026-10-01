// Map packs in the game: one search root per pack, a registry bank per pack generated from spice.json into section 12,
// FETXT titles, the cache root, the .csh guard at level start and the stale last-played level.
#include "levels/registry.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/crcsafe.h"
#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "erg/bank.h"
#include "erg/luagen.h"
#include "erg/names.h"
#include "levels/csh.h"
#include "levels/engine.h"
#include "levels/gate.h"
#include "levels/test.h"
#include "lua/sim/bridge_internal.h"
#include "melange/jlog.h"
#include "mods/thumper_internal.h"
#include "weapons/engine.h"
#include "xom/xom.h"

namespace melange::levels::registry {
namespace {
namespace fs = std::filesystem;
namespace eng = levels::engine;
namespace weng = weapons::engine;

constexpr const char* kFallbackLevel = "Multi.DinerMight";
constexpr const char* kFallbackPretty = "FETXT.Con.Diner.MissionName";
constexpr int kSettleFrames = 30;

struct Level {
    LevelInfo info{};
    fs::path root;             // the level root (assets/levels or the Test workspace)
    std::string modVersion;
    bool chunk = false;
    std::string chunkText;     // the last accepted chunk, served from the cache root
};

std::mutex g_mx;                                        // g_levels, g_sources
std::vector<Level> g_levels;
std::unordered_map<std::string, Source> g_sources;      // every declared or registered mod level key

Config g_cfg;
bool g_installed = false;
bool g_checked = false;
std::vector<roots::PackVerdict> g_verdicts;

bool g_packsDone = false, g_testRoot = false, g_cacheRoot = false;
int g_frontendFrames = 0;
uint32_t g_testBanks = 0, g_packs = 0, g_cshDeleted = 0;
double g_msRegister = 0;
std::unique_ptr<xom::Document> g_scripts;
bool g_scriptsTried = false;

bool g_lastPlayedDone = false;
uint64_t g_lastPlayedAt = 0;
int g_lastPlayedTries = 0;

std::vector<std::string> g_loaded;           // packs whose banks are in section 12 now, in load order
std::map<std::string, bool> g_live;          // packs changed at the menu this session: on or off now
uint32_t g_liveChanges = 0;

void Copy(char* dst, size_t n, const std::string& s) { strncpy_s(dst, n, s.c_str(), _TRUNCATE); }

fs::path GameDir() { return fs::path(game::GameDir()); }
fs::path CacheDir() { return GameDir() / L"Melange" / L"cache"; }
fs::path BankDir() { return CacheDir() / L"levels"; }
fs::path TestDir() { return GameDir() / L"Melange" / L"erg" / L"test"; }

const xom::Document* Scripts(std::string* err) {
    if (g_scripts) return g_scripts.get();
    if (g_scriptsTried) {
        *err = "Data/Tweak/SCRIPTS.XOM could not be read";
        return nullptr;
    }
    g_scriptsTried = true;
    std::ifstream f(GameDir() / L"Data" / L"Tweak" / L"SCRIPTS.XOM", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto doc = std::make_unique<xom::Document>();
    std::string perr;
    if (bytes.empty() || !xom::parse(bytes.data(), bytes.size(), *doc, &perr)) {
        *err = "Data/Tweak/SCRIPTS.XOM could not be read" + (perr.empty() ? "" : ": " + perr);
        return nullptr;
    }
    g_scripts = std::move(doc);
    return g_scripts.get();
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

bool EnsureCacheRoot() {
    if (g_cacheRoot) return true;
    std::error_code ec;
    fs::create_directories(CacheDir() / L"Maps", ec);
    fs::create_directories(BankDir(), ec);
    g_cacheRoot = eng::AddRoot(roots::kCacheRel);
    return g_cacheRoot;
}

bool KeyTaken(const std::string& key) { return eng::LevelDetails(key.c_str(), nullptr) || weng::Lookup(key.c_str()); }

std::string ScriptList(const std::vector<std::string>& s) {
    std::string o;
    for (const auto& x : s) o += (o.empty() ? "" : ",") + x;
    return o;
}

// Writes the bank, loads it into section 12 and adds the titles. Entries whose key already exists are left out.
int LoadEntries(const std::string& bankName, std::vector<erg::bank::Entry> entries, const std::vector<std::string>& titles,
                std::string* err) {
    std::vector<std::string> kept;
    std::vector<erg::bank::Entry> fresh;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (KeyTaken(entries[i].key)) {
            LOG_WARN("[levels] %s already exists in the data store; not registered again", entries[i].key.c_str());
            continue;
        }
        fresh.push_back(entries[i]);
        kept.push_back(titles[i]);
    }
    if (fresh.empty()) return 0;
    const xom::Document* doc = Scripts(err);
    if (!doc) return -1;
    std::vector<uint8_t> bytes = erg::bank::RegistryBank(*doc, fresh, err);
    if (bytes.empty()) {
        if (err->empty()) *err = "the registry bank could not be generated";
        return -1;
    }
    if (!WriteAtomic(BankDir() / game::Widen(bankName + ".XOM"), bytes)) {
        *err = "cannot write Melange/cache/levels/" + bankName + ".XOM";
        return -1;
    }
    const std::string rel = std::string(roots::kBankRel) + "/" + bankName + ".XOM";
    const int rc = eng::LoadDataBank(rel.c_str(), 12);
    if (rc != 0) {
        *err = "loading " + rel + " failed (" + std::to_string(rc) + ")";
        return -1;
    }
    int n = 0;
    std::set<std::string> named;   // a Survivor copy shares its level's title
    for (size_t i = 0; i < fresh.size(); ++i) {
        if (named.insert(fresh[i].frontendName).second && !eng::AddString(fresh[i].frontendName.c_str(), kept[i].c_str(), 12))
            LOG_WARN("[levels] the title of %s could not be added", fresh[i].key.c_str());
        n += eng::LevelDetails(fresh[i].key.c_str(), nullptr) ? 1 : 0;
    }
    return n;
}

// A pack level's sim script, read once with the frozen content set.
bool ReadLevelSim(const fs::path& modDir, const std::string& mod, const manifest::LevelDecl& d, const std::string& chunk,
                  simbridge::LevelSim* out, std::string* err) {
    std::error_code ec;
    const fs::path p = modDir / game::Widen(d.sim);
    const auto size = fs::file_size(p, ec);
    std::string text;
    if (!ec && size <= manifest::kMaxSimBytes) {
        std::ifstream f(p, std::ios::binary);
        text.resize(static_cast<size_t>(size));
        if (!f || (size && !f.read(text.data(), static_cast<std::streamsize>(size)))) ec = std::make_error_code(std::errc::io_error);
    }
    if (ec) {
        *err = d.sim + " could not be read";
        return false;
    }
    std::string why;
    if (size > manifest::kMaxSimBytes || !manifest::CheckSimText(text, &why)) {
        *err = d.sim + ": " + (why.empty() ? "the level script is larger than 256 KB" : why);
        return false;
    }
    out->mod = mod;
    out->slug = d.slug;
    out->stem = d.stem;
    out->key = erg::names::Key(d.stem);
    out->chunkName = "@" + mod + "/" + d.sim;
    out->text = std::move(text);
    out->knots = simbridge::LevelKnots(d.stem, chunk);
    return true;
}

// The engine runs the cache root's copy of a pack chunk: the generator's current form of an accepted chunk.
bool AcceptChunk(const std::string& stem, const fs::path& root, std::string* text, std::string* err) {
    std::string got, why, run;
    const std::string rel = stem + ".lub";
    if (!roots::ReadChunk(root / game::Widen(rel), &got)) {
        *err = "levels/" + rel + " could not be read";
        return false;
    }
    if (!erg::luagen::Upgrade(stem, got, &run, &why)) {
        *err = "levels/" + rel + " " + why;
        return false;
    }
    *text = std::move(run);
    return true;
}

bool ServeChunk(const std::string& stem, const std::string& text) {
    const fs::path p = CacheDir() / game::Widen(stem + ".lub");
    std::string have;
    if (roots::ReadChunk(p, &have) && have == text) return true;
    return WriteAtomic(p, std::vector<uint8_t>(text.begin(), text.end()));
}

void SetChunk(const std::string& stem, const std::string& text) {
    std::lock_guard lk(g_mx);
    for (auto& l : g_levels)
        if (stem == l.info.stem) l.chunkText = text;
}

// Serves the chunks, reads the level scripts into *sims (when given), then writes and loads the pack's bank and titles.
// The number of levels registered, or -1.
int LoadPack(const roots::PackVerdict& v, const fs::path& root, const fs::path& modDir, std::vector<simbridge::LevelSim>* sims,
             std::string* err) {
    std::vector<erg::bank::Entry> entries;
    std::vector<std::string> titles;
    for (const auto& d : v.levels) {
        std::string text;
        if (d.chunk) {
            if (!AcceptChunk(d.stem, root, &text, err)) return -1;
            if (!ServeChunk(d.stem, text)) {
                *err = "cannot write Melange/cache/" + d.stem + ".lub";
                return -1;
            }
            SetChunk(d.stem, text);
        }
        if (!d.sim.empty() && sims) {
            simbridge::LevelSim s;
            if (!ReadLevelSim(modDir, v.mod, d, text, &s, err)) return -1;
            sims->push_back(std::move(s));
        }
        erg::bank::Entry en;
        en.key = erg::names::Key(d.stem);
        en.stem = d.stem;
        en.frontendName = "FETXT." + d.stem;
        en.scripts = ScriptList(manifest::Scripts(d));
        entries.push_back(en);
        titles.push_back(d.title);
        if (d.survivor) {
            // No lock (a locked entry is left out of the picker) and the Prebuilt section, like the unlocked vanilla maps.
            en.key = manifest::TwinKey(d.stem);
            en.scripts = ScriptList(manifest::SurvivorScripts(d));
            en.levelType = 3;
            en.levelSection = 0;
            entries.push_back(en);
            titles.push_back(d.title);
        }
    }
    const int n = LoadEntries(erg::names::Prefix(v.mod) + "_REG", entries, titles, err);
    if (n >= 0 && static_cast<size_t>(n) < entries.size()) {
        *err = std::to_string(entries.size() - static_cast<size_t>(n)) + " of its levels did not register";
        return -1;
    }
    return n;
}

// A pack that fails to register is refused like one that failed the launch checks, so it leaves the content hash.
void Refuse(const std::string& mod, const std::string& why) {
    for (auto& v : g_verdicts)
        if (v.mod == mod) {
            v.ok = false;
            v.reason = why;
        }
    LOG_ERROR("[levels] %s: %s; the pack is not loaded", mod.c_str(), why.c_str());
    jlog::Rec("levels", jlog::Level::Error, "pack_failed").Str("mod", mod).Str("why", why);
    thumper::Rescan();
}

void MarkRegistered() {
    std::lock_guard lk(g_mx);
    for (auto& l : g_levels) l.info.registered = eng::LevelDetails(l.info.key, nullptr);
}

void RegisterPacks() {
    g_packsDone = true;
    std::vector<roots::PackVerdict> ok;
    for (const auto& v : g_verdicts)
        if (v.ok && !v.levels.empty()) ok.push_back(v);
    if (ok.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    internal::InstallHooks();

    struct Pack {
        roots::PackVerdict v;
        thumper::Entry e;
        std::string rel;
    };
    std::vector<Pack> packs;
    std::vector<std::string> rels;
    for (auto& v : ok) {
        Pack p{v, {}, {}};
        if (!thumper::FindEntry(v.mod, &p.e) || !p.e.sessionActive) continue;
        const fs::path root = fs::path(p.e.dir) / game::Widen(p.e.manifest.assetsRoot) / roots::kLevelDir;
        p.rel = roots::GameRelative(GameDir(), root);
        if (p.rel.empty()) {
            Refuse(v.mod, "the level root must be a game-relative path without '.' in it");
            continue;
        }
        rels.push_back(p.rel);
        packs.push_back(std::move(p));
    }
    std::error_code ec;
    const bool testRoot = fs::is_directory(TestDir(), ec);
    std::vector<std::string> failedRoots;
    for (const auto& r : roots::AddOrder(rels, testRoot, false)) {
        const bool added = eng::AddRoot(r.c_str());
        if (r == roots::kTestRel) g_testRoot = added;
        if (!added) {
            LOG_ERROR("[levels] adding the level root %s failed", r.c_str());
            failedRoots.push_back(r);
        }
    }
    const bool cacheRoot = EnsureCacheRoot();
    std::vector<simbridge::LevelSim> sims;

    for (auto& p : packs) {
        if (!cacheRoot) {
            Refuse(p.v.mod, "the level cache root could not be added");
            continue;
        }
        if (std::find(failedRoots.begin(), failedRoots.end(), p.rel) != failedRoots.end()) {
            Refuse(p.v.mod, "adding the level root " + p.rel + " failed");
            continue;
        }
        std::string err;
        std::vector<simbridge::LevelSim> packSims;
        const int n = LoadPack(p.v, GameDir() / game::Widen(p.rel), fs::path(p.e.dir), &packSims, &err);
        if (n < 0) {
            Refuse(p.v.mod, err);
        } else {
            ++g_packs;
            g_loaded.push_back(p.v.mod);
            for (auto& s : packSims) sims.push_back(std::move(s));
            LOG_INFO("[levels] %s: %d level(s) registered from %s", p.v.mod.c_str(), n, p.rel.c_str());
            jlog::Rec("levels", jlog::Level::Info, "pack").Str("mod", p.v.mod).Uint("levels", static_cast<uint64_t>(n));
        }
    }
    if (!sims.empty()) simbridge::SetLevelSims(std::move(sims));
    MarkRegistered();
    g_msRegister = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("[levels] registration took %.2f ms", g_msRegister);
}

// The first vanilla level of one of MissionService's random pools (locked levels are not in them).
std::string PoolFallback(uint32_t pool) {
    for (const auto& k : eng::PoolKeysAt(pool))
        if (!Lookup(k.c_str(), nullptr) && eng::LevelDetails(k.c_str(), nullptr)) return k;
    return {};
}

// The save keeps a last-played level per mode, and entering the mode makes it WXD.Level.Current: one that is not
// registered this launch (a removed pack's level or Survivor copy) would crash that mode's lobby start on both peers.
void CheckLastPlayed() {
    if (g_lastPlayedDone) return;
    const uint64_t now = GetTickCount64();
    if (g_lastPlayedAt && now - g_lastPlayedAt < 5000) return;
    g_lastPlayedAt = now;
    if (++g_lastPlayedTries > 120) {
        g_lastPlayedDone = true;
        return;
    }
    std::string last;
    if (!weng::TextOf("WXD.Level.LastPlayed", &last) || last.empty()) return;
    g_lastPlayedDone = true;
    struct Mode {
        const char* key;
        uint32_t pool;   // the mode's random pool in MissionService (Level_Type 0, 1, 2, 3, 11)
    };
    static constexpr Mode kModes[] = {{"WXD.Level.LastPlayed", 0x2c}, {"WXD.Level.LastPlayed.Dest", 0x8c},
                                      {"WXD.Level.LastPlayed.Stat", 0xa4}, {"WXD.Level.LastPlayed.Surv", 0xbc},
                                      {"WXD.Level.LastPlayed.Fort", 0xd4}};
    for (const Mode& m : kModes) {
        const bool main = m.pool == 0x2c;
        if (!main && (!weng::TextOf(m.key, &last) || last.empty())) continue;
        if (eng::LevelDetails(last.c_str(), nullptr)) continue;
        const std::string to = main ? kFallbackLevel : PoolFallback(m.pool);
        const bool ok = !to.empty() && eng::PostDataResource(m.key, to.c_str()) &&
                        (!main || eng::PostDataResource("WXD.Level.PrettyName", kFallbackPretty));
        LOG_WARN("[levels] %s '%s' is not registered; reset to %s: %s", m.key, last.c_str(), to.empty() ? "(none)" : to.c_str(),
                 ok ? "ok" : "FAILED");
        jlog::Rec("levels", ok ? jlog::Level::Warn : jlog::Level::Error, "last_played_reset")
            .Str("key", m.key).Str("was", last).Str("to", to).Bool("ok", ok);
    }
}

void OnStart(const LevelStart& s, void*) {
    if (s.source == Source::Vanilla) return;
    Level l;
    {
        std::lock_guard lk(g_mx);
        auto it = std::find_if(g_levels.begin(), g_levels.end(), [&](const Level& x) { return std::strcmp(x.info.key, s.key) == 0; });
        if (it == g_levels.end()) return;
        l = *it;
    }
    const std::string stem = l.info.stem;
    const fs::path xan = l.root / L"Maps" / game::Widen(stem + ".xan");
    if (l.info.source == Source::Pack && l.chunk) {
        std::string text, err;
        if (AcceptChunk(stem, l.root, &text, &err)) SetChunk(stem, text);
        else LOG_WARN("[levels] %s: %s; the last accepted chunk runs", stem.c_str(), err.c_str());
        if (!ServeChunk(stem, err.empty() ? text : l.chunkText))
            LOG_ERROR("[levels] %s: Melange/cache/%s.lub could not be written", stem.c_str(), stem.c_str());
    }
    const auto r = csh::Guard(xan, stem, BankDir(), {l.root / L"Maps", CacheDir() / L"Maps", TestDir() / L"Maps"});
    g_cshDeleted += r.deleted;
    if (!r.ok) LOG_WARN("[levels] shadow guard for %s: %s", stem.c_str(), r.error.c_str());
    else if (r.changed || r.deleted)
        LOG_INFO("[levels] %s: the .xan %s; %u shadow cache file(s) deleted", stem.c_str(), r.changed ? "changed" : "is newer",
                 r.deleted);
    jlog::Rec("levels", jlog::Level::Info, "csh_guard").Str("stem", stem).Bool("changed", r.changed)
        .Uint("deleted", r.deleted).Str("sha", r.sha.substr(0, 16));
}

Level MakeLevel(const std::string& key, const std::string& stem, const std::string& mod, const std::string& title,
                Source src, const fs::path& root, const std::string& version, bool chunk) {
    Level l;
    Copy(l.info.key, sizeof l.info.key, key);
    Copy(l.info.stem, sizeof l.info.stem, stem);
    Copy(l.info.mod, sizeof l.info.mod, mod);
    Copy(l.info.title, sizeof l.info.title, title);
    l.info.source = src;
    l.info.levelType = 0;
    l.info.themeType = 5;
    Copy(l.info.levelKind, sizeof l.info.levelKind, "multi");
    l.root = root;
    l.modVersion = version;
    l.chunk = chunk;
    return l;
}

void Declare(const Level& l) {
    std::lock_guard lk(g_mx);
    g_sources[l.info.key] = l.info.source;
    auto it = std::find_if(g_levels.begin(), g_levels.end(), [&](const Level& x) { return std::strcmp(x.info.key, l.info.key) == 0; });
    if (it == g_levels.end()) g_levels.push_back(l);
    else *it = l;
}

// A pack level and its Survivor copy, when it declares one.
void DeclarePack(const manifest::LevelDecl& d, const fs::path& root, const std::string& version, bool live) {
    Level l = MakeLevel(erg::names::Key(d.stem), d.stem, d.mod, d.title, Source::Pack, root, version, d.chunk);
    l.info.live = live;
    Declare(l);
    if (!d.survivor) return;
    Level t = MakeLevel(manifest::TwinKey(d.stem), d.stem, d.mod, d.title, Source::Pack, root, version,
                        d.chunk && manifest::kSurvivorRunsChunk);
    t.info.live = live;
    t.info.levelType = 3;
    Copy(t.info.levelKind, sizeof t.info.levelKind, "survivor");
    Declare(t);
}

std::vector<std::string> PackKeys(const manifest::LevelDecl& d) {
    std::vector<std::string> k = {erg::names::Key(d.stem)};
    if (d.survivor) k.push_back(manifest::TwinKey(d.stem));
    return k;
}
}  // namespace

std::vector<Refusal> CheckPacks(const std::vector<roots::PackInput>& inLoadOrder) {
    std::vector<Refusal> out;
    if (!g_checked) {
        if (!game::IsKnownBuild() || !config::GetBool("Levels", "Enabled", true)) return out;
        g_checked = true;
        const bool crc = assets::crcsafe::Available();
        g_verdicts = roots::CheckPacks(inLoadOrder, crc ? assets::crcsafe::Entries() : std::vector<assets::crcsafe::Entry>{}, crc);
        for (const auto& v : g_verdicts) {
            if (v.ok) {
                LOG_INFO("[levels] %s: %zu level(s) accepted", v.mod.c_str(), v.levels.size());
                continue;
            }
            LOG_WARN("[levels] %s: levels refused: %s", v.mod.c_str(), v.reason.c_str());
            jlog::Rec("levels", jlog::Level::Warn, "pack_refused").Str("mod", v.mod).Str("why", v.reason);
        }
        for (const auto& v : g_verdicts) {
            if (!v.ok) continue;
            std::string version;
            fs::path root;
            for (const auto& in : inLoadOrder)
                if (in.manifest && in.manifest->id == v.mod) {
                    version = in.manifest->version;
                    root = in.dir / game::Widen(in.manifest->assetsRoot) / roots::kLevelDir;
                }
            for (const auto& d : v.levels)
                DeclarePack(d, root, version, false);
        }
    }
    for (const auto& v : g_verdicts)
        if (!v.ok) out.push_back({v.mod, v.reason});
    return out;
}

void Install(const Config& cfg) {
    if (g_installed) return;
    g_installed = true;
    g_cfg = cfg;
    OnLevelStart(&OnStart, nullptr);
    gate::Install(cfg.online);
}

void OnFrame() {
    const bool front = eng::AtFrontend();
    g_frontendFrames = front ? g_frontendFrames + 1 : 0;
    if (front && g_frontendFrames >= kSettleFrames) {
        if (!g_packsDone && weng::AppReady() && weng::Drm()) RegisterPacks();
        if (g_packsDone) CheckLastPlayed();
    }
    test::OnFrame(front && g_frontendFrames >= kSettleFrames);
    gate::Tick();
}

bool HasModLevels() {
    std::lock_guard lk(g_mx);
    return !g_levels.empty();
}

int List(LevelInfo* out, int max, bool includeVanilla) {
    std::vector<LevelInfo> all;
    {
        std::lock_guard lk(g_mx);
        for (const auto& l : g_levels) all.push_back(l.info);
    }
    for (auto& i : all) i.registered = eng::LevelDetails(i.key, nullptr);
    if (includeVanilla) {
        for (const auto& k : eng::PoolKeys()) {
            if (Lookup(k.c_str(), nullptr)) continue;
            LevelInfo v{};
            Copy(v.key, sizeof v.key, k);
            eng::Details d;
            if (eng::LevelDetails(k.c_str(), &d)) {
                Copy(v.stem, sizeof v.stem, d.file);
                std::string title;
                if (!d.frontendName.empty() && weng::TextOf(d.frontendName.c_str(), &title)) Copy(v.title, sizeof v.title, title);
                v.levelType = static_cast<uint8_t>(d.levelType);
                v.themeType = static_cast<uint8_t>(d.themeType);
                v.registered = true;
                Copy(v.levelKind, sizeof v.levelKind, d.levelType == 3 ? "survivor" : "multi");
            }
            v.source = Source::Vanilla;
            all.push_back(v);
        }
    }
    const int total = static_cast<int>(all.size());
    for (int i = 0; out && i < total && i < max; ++i) out[i] = all[static_cast<size_t>(i)];
    return total;
}

bool Find(const char* key, LevelInfo* out) {
    if (!key) return false;
    std::lock_guard lk(g_mx);
    for (const auto& l : g_levels)
        if (std::strcmp(l.info.key, key) == 0) {
            if (out) *out = l.info;
            return true;
        }
    return false;
}

bool Lookup(const char* key, Source* source) {
    if (!key || std::strncmp(key, "Multi.", 6) != 0) return false;
    std::lock_guard lk(g_mx);
    auto it = g_sources.find(key);
    if (it == g_sources.end()) return false;
    if (source) *source = it->second;
    return true;
}

Stats GetStats() {
    Stats s{};
    std::lock_guard lk(g_mx);
    s.packs = g_packs;
    for (const auto& l : g_levels) (l.info.source == Source::Test ? s.testLevels : s.levels)++;
    s.cshDeleted = g_cshDeleted;
    s.heldStarts = gate::HeldStarts();
    s.msRegister = g_msRegister;
    for (const auto& [mod, on] : g_live) s.livePacks += on ? 1 : 0;
    s.livePackChanges = g_liveChanges;
    s.attractRefusals = test::AttractRefusals();
    return s;
}

const Config& Settings() { return g_cfg; }

bool RegisterTest(const char* stem, const char* title, char* err, size_t errLen) {
    return test::Register(stem, title, err, errLen);
}

bool Arm(const char* key, const ArmOptions& o) { return test::Arm(key, o); }
void Disarm() { test::Disarm("disarmed"); }
bool Armed(char* key, size_t keyLen) { return test::Armed(key, keyLen); }
const char* TakeOverride(const char* frontendKey) { return test::Take(frontendKey); }

bool Keep(const char* key, uint32_t) {
    Source s;
    if (!Lookup(key, &s)) return true;
    LevelInfo info{};
    return gate::KeepInList(s, gate::InLobby(), g_cfg.online, gate::MembersMatch(), Find(key, &info) && info.live);
}

bool KeepInPool(const char* key, uint32_t) {
    Source s;
    if (!Lookup(key, &s)) return true;
    return gate::KeepInPool(s, g_cfg.randomPool);
}

Online Status(const char* key) {
    if (!gate::InLobby()) return Online::NotInLobby;
    Source s = Source::Vanilla;
    const bool mod = Lookup(key, &s);
    if (!mod) return key && *key && !eng::LevelDetails(key, nullptr) ? Online::NotAllMatch : Online::Allowed;
    if (s == Source::Test) return Online::TestLevel;
    LevelInfo info{};
    if (Find(key, &info) && info.live) return Online::LivePack;
    return g_cfg.online && gate::MembersMatch() ? Online::Allowed : Online::NotAllMatch;
}

bool RegisterTestLevel(const std::string& stem, const std::string& title, std::string* err) {
    const std::string key = erg::names::Key(stem);
    const fs::path root = TestDir();
    std::error_code ec;
    if (!fs::is_regular_file(root / L"Maps" / game::Widen(stem + ".xan"), ec) ||
        !fs::is_regular_file(root / game::Widen(stem + ".XOM"), ec)) {
        *err = "not built: Melange/erg/test has no " + stem + " level files";
        return false;
    }
    const fs::path lub = root / game::Widen(stem + ".lub");
    if (!fs::is_regular_file(lub, ec)) {
        std::ofstream f(lub, std::ios::binary);
        f << erg::luagen::Stub(stem);
    }
    LevelInfo existing{};
    if (Find(key.c_str(), &existing)) {
        if (existing.source != Source::Test) {
            *err = key + " is a map pack level";
            return false;
        }
        eng::AddString(("FETXT." + stem).c_str(), title.c_str(), 12);
        std::lock_guard lk(g_mx);
        for (auto& l : g_levels)
            if (key == l.info.key) Copy(l.info.title, sizeof l.info.title, title);
        return true;
    }
    if (KeyTaken(key)) {
        *err = key + " already exists in the data store";
        return false;
    }
    internal::InstallHooks();
    if (!g_testRoot) {
        g_testRoot = eng::AddRoot(roots::kTestRel);
        if (!g_testRoot) {
            *err = "the Test workspace could not be added as a search root";
            return false;
        }
    }
    EnsureCacheRoot();
    erg::bank::Entry en;
    en.key = key;
    en.stem = stem;
    en.frontendName = "FETXT." + stem;
    en.scripts = "stdvs,wormpot," + stem;
    const int n = LoadEntries("ergtest_REG_" + std::to_string(g_testBanks + 1), {en}, {title}, err);
    if (n <= 0) {
        if (err->empty()) *err = key + " did not register";
        return false;
    }
    ++g_testBanks;
    Level l = MakeLevel(key, stem, "", title, Source::Test, root, "", true);
    l.info.registered = true;
    Declare(l);
    csh::Purge(stem, {root / L"Maps", CacheDir() / L"Maps"});
    return true;
}

namespace {
bool LoadBankFile(const std::string& bankName, const std::vector<std::pair<std::string, std::string>>& titles) {
    const std::string rel = std::string(roots::kBankRel) + "/" + bankName + ".XOM";
    const int rc = eng::LoadDataBank(rel.c_str(), 12);
    if (rc != 0) {
        LOG_ERROR("[levels] reloading %s failed (%d)", rel.c_str(), rc);
        return false;
    }
    bool ok = true;
    for (const auto& [stem, title] : titles) ok &= eng::AddString(("FETXT." + stem).c_str(), title.c_str(), 12);
    return ok;
}

// Clears section 12 and loads back every pack in g_loaded and every Test bank, then rebuilds the random pools, all in
// this frame: until the rebuild the pools still name the cleared keys.
bool ReloadAll() {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> byMod;
    std::vector<std::pair<std::string, std::string>> tests;
    {
        std::lock_guard lk(g_mx);
        for (const auto& l : g_levels) {
            if (l.info.source == Source::Test) tests.emplace_back(l.info.stem, l.info.title);
            else if (l.info.levelType != 3) byMod[l.info.mod].emplace_back(l.info.stem, l.info.title);
        }
    }
    if (!eng::ClearDataBank(12)) return false;
    bool ok = true;
    for (const auto& mod : g_loaded) ok &= LoadBankFile(erg::names::Prefix(mod) + "_REG", byMod[mod]);
    for (uint32_t i = 1; i <= g_testBanks; ++i) ok &= LoadBankFile("ergtest_REG_" + std::to_string(i), {});
    for (const auto& [stem, title] : tests) ok &= eng::AddString(("FETXT." + stem).c_str(), title.c_str(), 12);
    ok &= eng::RebuildPools();
    MarkRegistered();
    return ok;
}

void Forget(const std::string& mod) {
    std::lock_guard lk(g_mx);
    for (const auto& l : g_levels)
        if (mod == l.info.mod) g_sources.erase(l.info.key);
    std::erase_if(g_levels, [&](const Level& l) { return mod == l.info.mod; });
}

void RecheckLastPlayed() {
    g_lastPlayedDone = false;
    g_lastPlayedAt = 0;
    g_lastPlayedTries = 0;
    CheckLastPlayed();
}
}  // namespace

bool PacksReady() { return g_packsDone; }

bool Loaded(const std::string& mod) { return std::find(g_loaded.begin(), g_loaded.end(), mod) != g_loaded.end(); }

bool EnableLive(const std::string& mod, std::string* err) {
    if (Loaded(mod)) {
        *err = mod + " is already enabled";
        return false;
    }
    const auto snap = thumper::Snapshot();
    std::vector<roots::PackInput> in;
    const thumper::Entry* self = nullptr;
    for (const auto& e : snap) {
        if (e.manifest.id == mod) self = &e;
        if (e.manifest.id == mod || Loaded(e.manifest.id)) in.push_back({&e.manifest, e.dir});
    }
    if (!self) {
        *err = "no mod " + mod + " is installed";
        return false;
    }
    if (!assets::crcsafe::Available()) {
        *err = "the name table could not be verified";
        return false;
    }
    roots::PackVerdict verdict;
    bool found = false;
    for (const auto& v : roots::CheckPacks(in, assets::crcsafe::Entries(), true))
        if (v.mod == mod) {
            verdict = v;
            found = true;
        }
    if (!found || !verdict.ok || verdict.levels.empty()) {
        *err = found && !verdict.ok ? verdict.reason : mod + " has no levels to register";
        return false;
    }
    for (const auto& d : verdict.levels)
        for (const auto& k : PackKeys(d))
            if (KeyTaken(k)) {
                *err = k + " already exists in the data store";
                return false;
            }
    for (const auto& d : verdict.levels)
        if (!d.sim.empty()) {
            *err = mod + " has level scripts, which load only at launch; restart the game with it enabled";
            return false;
        }
    const fs::path root = fs::path(self->dir) / game::Widen(self->manifest.assetsRoot) / roots::kLevelDir;
    const std::string rel = roots::GameRelative(GameDir(), root);
    if (rel.empty()) {
        *err = "the level root must be a game-relative path without '.' in it";
        return false;
    }
    internal::InstallHooks();
    if (!EnsureCacheRoot() || !eng::AddRoot(rel.c_str())) {
        *err = "adding the level root " + rel + " failed";
        return false;
    }
    for (const auto& d : verdict.levels) DeclarePack(d, root, self->manifest.version, true);
    const int n = LoadPack(verdict, root, fs::path(self->dir), nullptr, err);
    if (n < 0) {
        Forget(mod);
        ReloadAll();
        return false;
    }
    g_loaded.push_back(mod);
    const bool pools = eng::RebuildPools();
    MarkRegistered();
    {
        std::lock_guard lk(g_mx);
        g_live[mod] = true;
        ++g_liveChanges;
    }
    LOG_INFO("[levels] %s: %d level(s) enabled at the menu from %s (offline only until restart); pools %s", mod.c_str(), n,
             rel.c_str(), pools ? "rebuilt" : "NOT rebuilt");
    jlog::Rec("levels", jlog::Level::Info, "pack_live").Str("mod", mod).Bool("on", true).Uint("levels", static_cast<uint64_t>(n));
    return true;
}

bool DisableLive(const std::string& mod, std::string* err) {
    if (!Loaded(mod)) {
        *err = mod + " is not enabled";
        return false;
    }
    std::erase(g_loaded, mod);
    Forget(mod);  // the pack's search root stays until exit (no removal); with its bank gone, nothing names its files
    const bool ok = ReloadAll();
    {
        std::lock_guard lk(g_mx);
        g_live[mod] = false;
        ++g_liveChanges;
    }
    RecheckLastPlayed();
    LOG_INFO("[levels] %s: disabled at the menu; section 12 reloaded %s", mod.c_str(), ok ? "ok" : "WITH ERRORS");
    jlog::Rec("levels", ok ? jlog::Level::Info : jlog::Level::Error, "pack_live").Str("mod", mod).Bool("on", false).Bool("ok", ok);
    if (!ok) *err = "the other packs could not all be reloaded; see Melange.log";
    return ok;
}

bool LiveChanged(const std::string& mod, bool* on) {
    std::lock_guard lk(g_mx);
    auto it = g_live.find(mod);
    if (it == g_live.end()) return false;
    if (on) *on = it->second;
    return true;
}
}  // namespace melange::levels::registry
