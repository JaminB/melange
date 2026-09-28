// Thumper: mod discovery, spice.json manifests, load order, enable/disable, Mods\thumper-state.json.
//
// Threading: Rescan() is safe to call from any thread (render/mirage/modfs.cpp's file watcher calls it
// from its own background thread), but the actual scan/parse/resolve/notify work always runs on the
// main thread: a call from elsewhere just requests it and OnFrame() picks it up next frame. Everything
// else here (SetEnabled, SetDeepDesert, the testcmd verbs, the overlay panels) is main-thread only, like
// the rest of the module system.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "lua/sandbox_internal.h"
#include "melange/bus.h"
#include "melange/draw.h"
#include "melange/jlog.h"
#include "melange/mods.h"
#include "melange/render.h"
#include "melange/sim.h"
#include "melange/testcmd.h"
#include "mods/thumper_internal.h"
#include "version.h"

namespace melange::thumper {
namespace {
std::mutex g_mx;
std::vector<Entry> g_entries;  // guarded by g_mx; replaced wholesale by DoRescan()
std::wstring g_modsDir;
int g_maxModMessages = 48;
bool g_autoGrantDeepDesert = false;
bool g_sessionFrozen = false;
std::atomic<bool> g_rescanRequested{false};

struct ChangeSub {
    int handle;
    mods::ChangeFn fn;
    void* user;
};
std::vector<ChangeSub> g_onChange;
int g_nextChangeHandle = 1;

bool g_messagesAttempted = false;
uint32_t g_lastRegistryCapacity = 0;
int g_stableFrames = 0;

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c < 128 ? c : '?'));
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

bool IsContentRelevant(const spice::Manifest& m) { return m.content || !m.entrySim.empty() || !m.messages.empty() || m.unsafe; }

bool FindEntryLocked(const std::string& id, Entry* out) {
    for (const Entry& e : g_entries)
        if (e.manifest.id == id) {
            *out = e;
            return true;
        }
    return false;
}

// -------------------------------------------------------------------------------------------
// Discovery: every immediate subfolder of the mods directory.
// -------------------------------------------------------------------------------------------
struct Candidate {
    std::string folderId;  // folder name, lowercased
    std::wstring dir;
};

std::vector<Candidate> ScanFolders() {
    std::vector<Candidate> out;
    if (g_modsDir.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_modsDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        out.push_back({Lower(Narrow(fd.cFileName)), g_modsDir + L"\\" + fd.cFileName});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.folderId < b.folderId; });
    return out;
}

// -------------------------------------------------------------------------------------------
// One-time migration of the M1-era [Mirage] DisabledMods list into thumper-state.json.
// -------------------------------------------------------------------------------------------
void MigrateDisabledMods() {
    if (Live().migratedDisabledMods) return;
    std::string disabled = config::GetString("Mirage", "DisabledMods", "");
    size_t p = 0;
    int migrated = 0;
    while (p <= disabled.size()) {
        size_t q = disabled.find(',', p);
        if (q == std::string::npos) q = disabled.size();
        std::string id = disabled.substr(p, q - p);
        id.erase(0, id.find_first_not_of(" \t"));
        if (auto e = id.find_last_not_of(" \t"); e != std::string::npos) id.resize(e + 1);
        if (!id.empty()) {
            Live().enabled[Lower(id)] = false;
            ++migrated;
        }
        p = q + 1;
    }
    Live().migratedDisabledMods = true;
    Save();
    if (migrated) {
        config::SetString("Mirage", "DisabledMods", "");
        LOG_INFO("[thumper] migrated %d id(s) from [Mirage] DisabledMods into thumper-state.json", migrated);
    }
}

// -------------------------------------------------------------------------------------------
// Scan + parse + resolve, then merge into g_entries. Main thread only.
// -------------------------------------------------------------------------------------------
void DoRescan() {
    std::vector<Candidate> candidates = ScanFolders();
    std::vector<spice::Manifest> manifests;
    std::vector<Entry> broken;  // folders whose spice.json failed to parse: shown Incompatible, no graph slot
    manifests.reserve(candidates.size());

    for (const Candidate& c : candidates) {
        spice::Manifest m;
        std::vector<spice::Error> errs;
        if (spice::Parse(c.dir, &m, &errs)) {
            manifests.push_back(std::move(m));
            continue;
        }
        Entry e;
        e.manifest.id = c.folderId;
        e.manifest.name = c.folderId;
        e.dir = c.dir;
        e.state = mods::State::Incompatible;
        std::string text;
        for (const spice::Error& er : errs) {
            if (!text.empty()) text += "; ";
            if (er.line) text += std::to_string(er.line) + ":" + std::to_string(er.col) + " ";
            text += er.text;
        }
        e.reason = text.empty() ? "invalid spice.json" : text;
        broken.push_back(std::move(e));
    }

    // Decision Q5: a newly discovered mod is enabled unless its manifest says otherwise; record the
    // choice immediately so it is only made once, the first time Thumper ever sees this id.
    bool stateChanged = false;
    for (const spice::Manifest& m : manifests) {
        if (!Live().enabled.count(m.id)) {
            Live().enabled[m.id] = m.defaultEnabled;
            stateChanged = true;
        }
    }
    std::set<std::string> userEnabled;
    for (const auto& [id, on] : Live().enabled)
        if (on) userEnabled.insert(id);

    std::vector<std::pair<std::string, std::string>> pins;
    for (const PinEntry& p : Live().pins) {
        if (!p.before.empty()) pins.emplace_back(p.id, p.before);  // p.id loads before p.before
        if (!p.after.empty()) pins.emplace_back(p.after, p.id);    // p.after loads before p.id
    }

    std::vector<spice::Resolved> resolved = spice::Resolve(manifests, userEnabled, MELANGE_VERSION, pins);

    std::vector<Entry> next;
    next.reserve(resolved.size() + broken.size());
    for (const spice::Resolved& r : resolved) {
        const spice::Manifest* m = nullptr;
        for (const spice::Manifest& x : manifests)
            if (x.id == r.id) {
                m = &x;
                break;
            }
        if (!m) continue;
        Entry e;
        e.manifest = *m;
        e.dir = m->dir;
        e.order = r.order;
        e.contentRelevant = IsContentRelevant(*m);
        e.authorsJoined = Join(m->authors, ", ");
        bool loadsNow = r.state == mods::State::Enabled || r.state == mods::State::PendingConsent;

        Entry prev;
        bool hadPrev;
        {
            std::lock_guard lk(g_mx);
            hadPrev = FindEntryLocked(r.id, &prev);
        }
        if (e.contentRelevant && g_sessionFrozen && hadPrev) {
            e.sessionActive = prev.sessionActive;  // frozen: content-relevant mods don't change mid-session
            e.state = loadsNow == e.sessionActive ? r.state : mods::State::RestartRequired;
            e.reason = loadsNow == e.sessionActive ? r.reason : "enable/disable takes effect next launch";
        } else {
            e.sessionActive = loadsNow;
            e.state = r.state;
            e.reason = r.reason;
        }
        e.deepDesertGranted = IsGranted(e);
        if (m->unsafe && e.state == mods::State::PendingConsent && !e.deepDesertGranted) {
            if (g_autoGrantDeepDesert) {
                DeepDesertRecord dr;
                dr.granted = true;
                dr.grantHash = GrantHash(*m);
                dr.author = m->authors.empty() ? "" : m->authors.front();
                Live().deepDesert[m->id] = dr;
                stateChanged = true;
                e.deepDesertGranted = true;
                e.state = mods::State::Enabled;
                e.sessionActive = true;
            } else if (!hadPrev || prev.state != mods::State::PendingConsent) {
                RequestConsent(r.id);  // newly pending-consent this pass: pop the modal once
            }
        }
        next.push_back(std::move(e));
    }
    for (Entry& e : broken) next.push_back(std::move(e));
    std::sort(next.begin(), next.end(), [](const Entry& a, const Entry& b) {
        bool ap = a.order >= 0, bp = b.order >= 0;
        if (ap != bp) return ap;
        if (ap) return a.order < b.order;
        return a.manifest.id < b.manifest.id;
    });

    std::vector<ChangeSub> subs;
    {
        std::lock_guard lk(g_mx);
        g_entries = std::move(next);
        g_sessionFrozen = true;
        subs = g_onChange;  // copy so a handler adding/removing a subscription is safe
    }
    if (stateChanged) Save();
    for (const ChangeSub& s : subs) s.fn(s.user);
}

// -------------------------------------------------------------------------------------------
// Mod message registration: at the first Frame where the message registry has been stable for 30
// frames, register every enabled content mod's declared names, in load order, then freeze.
// -------------------------------------------------------------------------------------------
void RegisterModMessagesOnce() {
    std::vector<Entry> snap = Snapshot();
    bool any = false;
    for (const Entry& e : snap)
        if (e.sessionActive && e.contentRelevant && !e.manifest.messages.empty()) any = true;
    if (!any) return;
    int registered = 0;
    for (const Entry& e : snap) {
        if (!(e.sessionActive && e.contentRelevant)) continue;
        for (const std::string& name : e.manifest.messages) {
            if (registered >= g_maxModMessages) {
                LOG_WARN("[thumper] %s: message budget (%d) reached, '%s' not registered", e.manifest.id.c_str(),
                          g_maxModMessages, name.c_str());
                continue;
            }
            // Checked against the live registry, not a hard-coded name list: a mod message may never
            // collide with a vanilla one, and only the running engine truly knows the vanilla set.
            if (bus::IdOf(name) != bus::kInvalidId) {
                LOG_WARN("[thumper] %s: message '%s' collides with a vanilla name, skipped", e.manifest.id.c_str(), name.c_str());
                continue;
            }
            uint16_t id = 0;
            if (sim::RegisterModMessage(name.c_str(), &id)) {
                ++registered;
                jlog::Rec("thumper", jlog::Level::Info, "register_message").Str("mod", e.manifest.id).Str("name", name).Uint("id", id);
            } else {
                LOG_WARN("[thumper] %s: '%s' not registered (sim bridge unavailable)", e.manifest.id.c_str(), name.c_str());
            }
        }
    }
    sim::FreezeModMessages();
    LOG_INFO("[thumper] mod message registration done: %d name(s) registered", registered);
}

void OnFrame() {
    if (g_rescanRequested.exchange(false)) DoRescan();

    if (g_messagesAttempted) return;
    if (!bus::RegistryReady()) {
        g_stableFrames = 0;
        return;
    }
    uint32_t cap = static_cast<uint32_t>(bus::Capacity());
    if (cap == g_lastRegistryCapacity) {
        ++g_stableFrames;
    } else {
        g_lastRegistryCapacity = cap;
        g_stableFrames = 0;
    }
    if (g_stableFrames < 30) return;
    g_messagesAttempted = true;
    RegisterModMessagesOnce();
}

void ToModInfo(const Entry& e, mods::ModInfo* out) {
    *out = mods::ModInfo{};
    out->id = e.manifest.id.c_str();
    out->name = e.manifest.name.c_str();
    out->version = e.manifest.version.c_str();
    out->authors = e.authorsJoined.c_str();
    out->dir = e.dir.c_str();
    out->kind = e.manifest.content ? mods::Kind::Content : mods::Kind::ClientOnly;
    out->state = e.state;
    out->reason = e.reason.c_str();
    out->implicitManifest = e.manifest.implicit;
    out->hasClient = !e.manifest.entryClient.empty();
    out->hasSim = !e.manifest.entrySim.empty();
    out->unsafe = e.manifest.unsafe;
    out->unsafeGranted = e.deepDesertGranted;
    out->order = e.order;
}

const char* StateNameImpl(mods::State s) {
    switch (s) {
        case mods::State::Enabled: return "enabled";
        case mods::State::Disabled: return "disabled";
        case mods::State::Blocked: return "blocked";
        case mods::State::PendingConsent: return "pending-consent";
        case mods::State::Incompatible: return "incompatible";
        case mods::State::RestartRequired: return "restart-required";
    }
    return "?";
}

// -------------------------------------------------------------------------------------------
// testcmd verbs
// -------------------------------------------------------------------------------------------
bool VerbList(std::string_view, void*) {
    for (const Entry& e : Snapshot())
        LOG_INFO("[thumper] %-24s %-10s %-16s order=%-3d %s", e.manifest.id.c_str(), e.manifest.version.c_str(),
                 StateNameImpl(e.state), e.order, e.reason.c_str());
    return true;
}

bool SplitIdAndFlag(std::string_view args, std::string* id, bool* flag) {
    size_t sp = args.find(' ');
    if (sp == std::string_view::npos) return false;
    *id = std::string(args.substr(0, sp));
    std::string_view rest = args.substr(sp + 1);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    if (rest.empty()) return false;
    *flag = rest.front() != '0';
    return true;
}

bool VerbEnable(std::string_view args, void*) {
    std::string id;
    bool on = false;
    if (!SplitIdAndFlag(args, &id, &on)) return false;
    return SetEnabled(id, on);
}

bool VerbGrant(std::string_view args, void*) {
    std::string id;
    bool on = false;
    if (!SplitIdAndFlag(args, &id, &on)) return false;
    return SetDeepDesert(id, on);
}

bool VerbReload(std::string_view args, void*) {
    std::string id(args);
    while (!id.empty() && id.back() == ' ') id.pop_back();
    if (id.empty()) return false;
    return sandbox::ReloadMod(id.c_str());
}

bool VerbRescan(std::string_view, void*) {
    Rescan();
    return true;
}

void DrawMarkerCallback(render::Stage, void*) { DrawDeepDesertMarker(); }
}  // namespace

// ---------------------------------------------------------------------------------------------
// Public (to the rest of A) plumbing, declared in thumper_internal.h.
// ---------------------------------------------------------------------------------------------
std::vector<Entry> Snapshot() {
    std::lock_guard lk(g_mx);
    return g_entries;
}

bool FindEntry(const std::string& id, Entry* out) {
    std::lock_guard lk(g_mx);
    return FindEntryLocked(id, out);
}

std::vector<SessionRoot> ActiveRoots() {
    std::vector<Entry> snap = Snapshot();
    std::vector<SessionRoot> out;
    for (const Entry& e : snap)
        if (e.sessionActive) out.push_back({e.manifest.id, e.dir});
    return out;
}

void Rescan() {
    unsigned long mainTid = events::MainThreadId();
    if (mainTid != 0 && GetCurrentThreadId() != mainTid) {
        g_rescanRequested.store(true, std::memory_order_relaxed);
        return;
    }
    DoRescan();
}

bool SetEnabled(const std::string& id, bool on) {
    Live().enabled[id] = on;
    Save();
    Rescan();
    Entry e;
    if (FindEntry(id, &e) && !e.contentRelevant) {
        if (on)
            sandbox::LoadMod(id.c_str());
        else
            sandbox::UnloadMod(id.c_str());
    }
    jlog::Rec("thumper", jlog::Level::Info, on ? "enable" : "disable").Str("id", id);
    return true;
}

bool SetDeepDesert(const std::string& id, bool granted) {
    Entry e;
    if (!FindEntry(id, &e)) return false;
    DeepDesertRecord r = Live().deepDesert.count(id) ? Live().deepDesert[id] : DeepDesertRecord{};
    r.granted = granted;
    if (granted) r.grantHash = GrantHash(e.manifest);
    Live().deepDesert[id] = r;
    Save();
    jlog::Rec("thumper", jlog::Level::Info, granted ? "deep_desert_grant" : "deep_desert_revoke").Str("id", id);
    Rescan();
    return true;
}
}  // namespace melange::thumper

// ---------------------------------------------------------------------------------------------
// melange::mods (frozen public API): the mod-list half. Handshake (E) implements the content-identity
// half (LocalContent/Peers/SimAllowedThisMatch) in src/mods/handshake.cpp, untouched by A.
// ---------------------------------------------------------------------------------------------
namespace melange::mods {
int List(ModInfo* out, int max) {
    std::vector<thumper::Entry> snap = thumper::Snapshot();
    if (!out) return static_cast<int>(snap.size());
    int n = 0;
    for (const thumper::Entry& e : snap) {
        if (n >= max) break;
        thumper::ToModInfo(e, &out[n]);
        ++n;
    }
    return n;
}

bool Find(const char* id, ModInfo* out) {
    if (!id || !out) return false;
    thumper::Entry e;
    if (!thumper::FindEntry(id, &e)) return false;
    thumper::ToModInfo(e, out);
    return true;
}

bool SetEnabled(const char* id, bool on) { return id && thumper::SetEnabled(id, on); }
bool SetDeepDesert(const char* id, bool granted) { return id && thumper::SetDeepDesert(id, granted); }

const wchar_t* ModsDir() {
    static std::wstring s;
    std::lock_guard lk(thumper::g_mx);
    s = thumper::g_modsDir;
    return s.c_str();
}

int OnChange(ChangeFn fn, void* user) {
    std::lock_guard lk(thumper::g_mx);
    int h = thumper::g_nextChangeHandle++;
    thumper::g_onChange.push_back({h, fn, user});
    return h;
}

void RemoveOnChange(int handle) {
    std::lock_guard lk(thumper::g_mx);
    auto& v = thumper::g_onChange;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const thumper::ChangeSub& s) { return s.handle == handle; }), v.end());
}
}  // namespace melange::mods

namespace melange::thumper {
namespace {
class Thumper final : public melange::Module {
public:
    const char* Name() const override { return "Thumper"; }
    const char* Description() const override { return "mod manager: spice.json manifests, load order, Mods page"; }
    int Order() const override { return 36; }

    bool Install() override {
        std::string modsDirCfg = String("ModsDir", "Mods");
        g_maxModMessages = Int("MaxModMessages", 48);
        g_autoGrantDeepDesert = Bool("AutoGrantDeepDesert", false);
        std::wstring dir(modsDirCfg.begin(), modsDirCfg.end());
        if (dir.size() < 2 || (dir[1] != L':' && dir[0] != L'\\')) dir = melange::game::GameDir() + L"\\" + dir;
        {
            std::lock_guard lk(g_mx);
            g_modsDir = dir;
        }
        CreateDirectoryW(dir.c_str(), nullptr);
        Load();
        MigrateDisabledMods();
        Rescan();
        RegisterPanels();
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        melange::draw::AddDrawCallback(melange::render::Stage::Hud, &DrawMarkerCallback, nullptr);
        melange::testcmd::Register("thumper.list", &VerbList);
        melange::testcmd::Register("thumper.enable", &VerbEnable);
        melange::testcmd::Register("thumper.grant", &VerbGrant);
        melange::testcmd::Register("thumper.reload", &VerbReload);
        melange::testcmd::Register("thumper.rescan", &VerbRescan);
        LOG_INFO("[thumper] ready: %zu mod(s) under %s", Snapshot().size(), Narrow(dir).c_str());
        return true;
    }

private:
    std::string String(const char* key, const char* def) const {
        melange::config::EnsureKey(Name(), key, def);
        return melange::config::GetString(Name(), key, def);
    }
};
}  // namespace

MELANGE_MODULE(Thumper);
}  // namespace melange::thumper
