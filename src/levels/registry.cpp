// Map packs in the game: one search root per pack, a registry bank per pack generated from spice.json into section 12,
// FETXT titles, the cache root, the .csh guard at level start and the stale last-played level.
#include "levels/registry.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
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
    for (size_t i = 0; i < fresh.size(); ++i) {
        if (!eng::AddString(fresh[i].frontendName.c_str(), kept[i].c_str(), 12))
            LOG_WARN("[levels] the title of %s could not be added", fresh[i].key.c_str());
        n += eng::LevelDetails(fresh[i].key.c_str(), nullptr) ? 1 : 0;
    }
    return n;
}

// The engine runs the cache root's copy of a pack chunk, written only from text the generator could have produced.
bool AcceptChunk(const std::string& stem, const fs::path& root, std::string* text, std::string* err) {
    std::string got, why;
    const std::string rel = stem + ".lub";
    if (!roots::ReadChunk(root / game::Widen(rel), &got)) {
        *err = "levels/" + rel + " could not be read";
        return false;
    }
    if (!erg::luagen::IsGenerated(stem, got, &why)) {
        *err = "levels/" + rel + " " + why;
        return false;
    }
    *text = std::move(got);
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

    for (auto& p : packs) {
        if (!cacheRoot) {
            Refuse(p.v.mod, "the level cache root could not be added");
            continue;
        }
        if (std::find(failedRoots.begin(), failedRoots.end(), p.rel) != failedRoots.end()) {
            Refuse(p.v.mod, "adding the level root " + p.rel + " failed");
            continue;
        }
        const fs::path root = GameDir() / game::Widen(p.rel);
        std::vector<erg::bank::Entry> entries;
        std::vector<std::string> titles;
        std::string err;
        for (const auto& d : p.v.levels) {
            if (d.chunk && err.empty()) {
                std::string text;
                if (!AcceptChunk(d.stem, root, &text, &err)) break;
                if (!ServeChunk(d.stem, text)) {
                    err = "cannot write Melange/cache/" + d.stem + ".lub";
                    break;
                }
                SetChunk(d.stem, text);
            }
            erg::bank::Entry en;
            en.key = erg::names::Key(d.stem);
            en.stem = d.stem;
            en.frontendName = "FETXT." + d.stem;
            en.scripts = ScriptList(manifest::Scripts(d));
            entries.push_back(en);
            titles.push_back(d.title);
        }
        if (!err.empty()) {
            Refuse(p.v.mod, err);
            continue;
        }
        const int n = LoadEntries(erg::names::Prefix(p.v.mod) + "_REG", entries, titles, &err);
        if (n < 0) {
            Refuse(p.v.mod, err);
        } else if (static_cast<size_t>(n) < entries.size()) {
            Refuse(p.v.mod, std::to_string(entries.size() - static_cast<size_t>(n)) + " of its levels did not register");
        } else {
            ++g_packs;
            LOG_INFO("[levels] %s: %d level(s) registered from %s", p.v.mod.c_str(), n, p.rel.c_str());
            jlog::Rec("levels", jlog::Level::Info, "pack").Str("mod", p.v.mod).Uint("levels", static_cast<uint64_t>(n));
        }
    }
    MarkRegistered();
    g_msRegister = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("[levels] registration took %.2f ms", g_msRegister);
}

// A last-played level that is not registered this launch would crash a lobby start on both peers.
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
    if (eng::LevelDetails(last.c_str(), nullptr)) return;
    const bool ok = eng::PostDataResource("WXD.Level.LastPlayed", kFallbackLevel) &&
                    eng::PostDataResource("WXD.Level.PrettyName", kFallbackPretty);
    LOG_WARN("[levels] the last-played level '%s' is not registered; reset to %s: %s", last.c_str(), kFallbackLevel,
             ok ? "ok" : "FAILED");
    jlog::Rec("levels", ok ? jlog::Level::Warn : jlog::Level::Error, "last_played_reset").Str("was", last).Bool("ok", ok);
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
                Declare(MakeLevel(erg::names::Key(d.stem), d.stem, d.mod, d.title, Source::Pack, root, version, d.chunk));
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
    return s;
}

const Config& Settings() { return g_cfg; }

bool RegisterTest(const char* stem, const char* title, char* err, size_t errLen) {
    return test::Register(stem, title, err, errLen);
}

bool Arm(const char* key, int timeoutS) { return test::Arm(key, timeoutS); }
void Disarm() { test::Disarm("disarmed"); }
bool Armed(char* key, size_t keyLen) { return test::Armed(key, keyLen); }
const char* TakeOverride(const char* frontendKey) { return test::Take(frontendKey); }

bool Keep(const char* key, uint32_t) {
    Source s;
    if (!Lookup(key, &s)) return true;
    return gate::KeepInList(s, gate::InLobby(), g_cfg.online, gate::MembersMatch());
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
}  // namespace melange::levels::registry
