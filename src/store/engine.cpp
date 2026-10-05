// The Store engine: the index in memory, the worker thread that owns all network and disk work, and the actions the
// overlay page and Oasis share. Everything game- or launcher-specific goes through the Host.
#include "store/store.h"

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "core/log.h"
#include "store/compat.h"
#include "store/fetch.h"
#include "store/install.h"
#include "tools/hash.h"
#include "tools/json_mini.h"

namespace melange::store {
const char* const kDefaultIndex = "https://raw.githubusercontent.com/JaminB/melange-plugins/main/index.json";

namespace {
using Settings = Config;

struct StepData {
    Plugin plugin;   // without versions
    Version version;
    std::string url;
};
struct Task {
    enum Kind { Fetch, Shots, Add, Drop, Reconcile } kind = Fetch;
    std::string id;
    std::vector<StepData> steps;
    bool enable = true, deleteData = false;
    // Reconcile (the compatibility sweep's Store half); keepGenerated also applies to the Drops it makes.
    std::vector<compat::Finding> found;
    bool fetch = false, keepGenerated = false;
    std::string melange;   // "" = the host's version
};

Settings g_set;
std::atomic<bool> g_active{false};
install::Paths g_paths;

std::mutex g_mx;
std::shared_ptr<const Index> g_index;
std::string g_fetchedAt, g_fetchError;
bool g_offline = false, g_fetching = false, g_rollback = false, g_fetchedThisSession = false, g_fetchRequested = false;
bool g_busy = false;
install::Db g_db;
std::vector<install::Pending> g_pending;
Job g_job;
std::map<std::string, std::string> g_rowError;
std::vector<std::string> g_notices;
std::set<std::string> g_shotRequested;
int g_shotsReady = 0;
std::string g_lastShotId;
int g_lastShotN = 0;
std::string g_gate;
std::set<std::string> g_updates;
bool g_updatesDirty = true;
std::string g_indexText;
Host* g_host = nullptr;

std::deque<Task> g_tasks;
std::condition_variable g_cv;
bool g_stop = false, g_workerStarted = false;
std::atomic<bool> g_cancel{false};
ULONGLONG g_lastProgress = 0;

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string LocalStamp(const SYSTEMTIME& t) {
    char buf[32];
    snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return buf;
}

std::string NowLocal() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return LocalStamp(t);
}

Env MakeEnv(bool rollback) { return Env{g_host->MelangeVersion(), g_host->GameBuild(), rollback}; }

void DeleteData(const std::string& id) {
    g_host->DeleteData(id);
    LOG_INFO("[store] deleted the settings and saved data of %s", id.c_str());
}

std::map<std::string, LocalMod> LocalMods() {
    std::map<std::string, LocalMod> out;
    if (g_host)
        for (LocalMod& m : g_host->InstalledMods()) out[m.id] = std::move(m);
    return out;
}

// ---- state helpers ------------------------------------------------------------------------------------------
void SetJob(const std::string& phase, const std::string& id, const std::string& version, const std::string& message,
            uint64_t bytes = 0, uint64_t total = 0) {
    {
        std::lock_guard lk(g_mx);
        g_job = Job{phase, id, version, message, bytes, total};
    }
    PublishState();
}

void FailJob(const std::string& id, const std::string& version, const std::string& why) {
    LOG_WARN("[store] %s %s: %s", id.c_str(), version.c_str(), why.c_str());
    {
        std::lock_guard lk(g_mx);
        g_rowError[id] = why;
    }
    SetJob("error", id, version, why);
}

bool PendingFor(const std::string& id) {
    for (const install::Pending& p : g_pending)
        if (p.id == id) return true;
    return false;
}

void AddPending(const install::Pending& op) {
    std::lock_guard lk(g_mx);
    std::erase_if(g_pending, [&](const install::Pending& p) { return p.id == op.id; });
    g_pending.push_back(op);
    install::SavePending(g_paths, g_pending);
}

void Record(const std::string& id, const Version& v) {
    std::lock_guard lk(g_mx);
    g_db.mods[id] = install::Record{v.version, v.sha256, install::NowIso(), g_index ? g_index->serial : 0};
    install::SaveDb(g_paths, g_db);
}

std::string LastIndexUrl() {
    FILE* f = _wfopen((g_paths.root + L"\\index.url").c_str(), L"rb");
    if (!f) return {};
    char buf[1024] = {};
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return std::string(buf, n);
}

bool LoadCachedIndex(Index* out, std::string* when) {
    if (LastIndexUrl() != g_set.indexUrl) return false;
    const std::wstring path = g_paths.root + L"\\index.json";
    std::string text;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() <= kMaxIndexBytes) text.append(buf, n);
    fclose(f);
    std::string err;
    if (!ParseIndex(text, out, &err)) return false;
    {
        std::lock_guard lk(g_mx);
        g_indexText = text;
    }
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        FILETIME local;
        SYSTEMTIME t;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &t);
        *when = LocalStamp(t);
    }
    return true;
}

// ---- worker -------------------------------------------------------------------------------------------------
fetch::Options BaseOptions() {
    fetch::Options o;
    o.userAgent = "Melange/" + g_host->MelangeVersion();
    o.cancel = &g_cancel;
    return o;
}

void DoFetch() {
    {
        std::lock_guard lk(g_mx);
        g_fetching = true;
        g_job = Job{"fetching", "", "", "", 0, 0};
    }
    PublishState();
    fetch::StringSink sink;
    fetch::Options o = BaseOptions();
    o.cancel = nullptr;
    o.cap = kMaxIndexBytes;
    o.totalMs = 30000;
    std::string err;
    Index idx;
    bool ok = fetch::Get(g_set.indexUrl, sink, o, &err);
    if (ok && !ParseIndex(sink.data, &idx, &err)) {
        ok = false;
        err = "the store list is invalid: " + err;
    }
    if (ok) {
        const bool newList = LastIndexUrl() != g_set.indexUrl;
        install::EnsureDirs(g_paths);
        install::WriteFileAtomic(g_paths.root + L"\\index.json", sink.data);
        install::WriteFileAtomic(g_paths.root + L"\\index.url", g_set.indexUrl);
        LOG_INFO("[store] fetched the list: %zu plugin(s), serial %lld%s", idx.plugins.size(), idx.serial,
                 idx.skipped.empty() ? "" : (", " + std::to_string(idx.skipped.size()) + " entr(ies) skipped").c_str());
        for (const std::string& s : idx.skipped) LOG_WARN("[store] skipped %s", s.c_str());
        std::lock_guard lk(g_mx);
        const bool track = SchemeOf(g_set.indexUrl) != Scheme::File;
        if (newList && g_db.serialSeen) {
            g_db.serialSeen = 0;
            install::SaveDb(g_paths, g_db);
        }
        g_rollback = track && idx.serial < g_db.serialSeen;
        if (g_rollback) {
            LOG_WARN("[store] the list's serial %lld is lower than %lld seen before: updates disabled", idx.serial, g_db.serialSeen);
        } else if (track && idx.serial > g_db.serialSeen) {
            g_db.serialSeen = idx.serial;
            install::SaveDb(g_paths, g_db);
        }
        g_index = std::make_shared<const Index>(std::move(idx));
        g_indexText = sink.data;
        g_fetchedAt = NowLocal();
        g_fetchError.clear();
        g_offline = false;
        g_fetchedThisSession = true;
    } else {
        LOG_WARN("[store] fetching the list failed: %s", err.c_str());
        Index cached;
        std::string when;
        bool haveIndex;
        {
            std::lock_guard lk(g_mx);
            haveIndex = g_index != nullptr;
        }
        const bool haveCache = !haveIndex && LoadCachedIndex(&cached, &when);
        std::lock_guard lk(g_mx);
        g_fetchError = err;
        g_offline = true;
        if (haveCache) {
            g_rollback = SchemeOf(g_set.indexUrl) != Scheme::File && cached.serial < g_db.serialSeen;
            g_index = std::make_shared<const Index>(std::move(cached));
            g_fetchedAt = when;
        }
    }
    {
        std::lock_guard lk(g_mx);
        g_fetching = false;
        g_updatesDirty = true;
        g_job = Job{};
    }
    PublishState();
}

void DoShots(const std::string& id) {
    std::shared_ptr<const Index> idx;
    {
        std::lock_guard lk(g_mx);
        idx = g_index;
    }
    const Plugin* p = idx ? FindPlugin(*idx, id) : nullptr;
    if (!p) return;
    install::EnsureDirs(g_paths);
    for (size_t i = 0; i < p->screenshots.size(); ++i) {
        const Shot& s = p->screenshots[i];
        const std::wstring path = ShotPath(id, static_cast<int>(i + 1));
        if (!path.empty()) continue;
        std::string url, err;
        if (!ResolveUrl(g_set.indexUrl, s.path, &url, &err)) {
            LOG_WARN("[store] %s screenshot %zu: %s", id.c_str(), i + 1, err.c_str());
            continue;
        }
        std::string lower = s.path;
        for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        const char* ext = lower.ends_with(".png") ? ".png" : (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) ? ".jpg" : nullptr;
        if (!ext) continue;
        fetch::StringSink sink;
        fetch::Options o = BaseOptions();
        o.cancel = nullptr;
        o.cap = std::min<uint64_t>(kMaxShotBytes, s.size);
        o.totalMs = 30000;
        if (!fetch::Get(url, sink, o, &err)) {
            LOG_WARN("[store] %s screenshot %zu: %s", id.c_str(), i + 1, err.c_str());
            continue;
        }
        if (sink.data.size() != s.size || hashutil::Sha256Hex(sink.data.data(), sink.data.size()) != s.sha256) {
            LOG_WARN("[store] %s screenshot %zu does not match the store's record", id.c_str(), i + 1);
            continue;
        }
        install::WriteFileAtomic(g_paths.Cache() + L"\\" + W(s.sha256) + W(ext), sink.data);
        {
            std::lock_guard lk(g_mx);
            ++g_shotsReady;
            g_lastShotId = id;
            g_lastShotN = static_cast<int>(i + 1);
        }
        PublishState();
    }
}

bool PlaceStep(const StepData& d, bool enable, const std::string& melange, std::string* message) {
    const std::string& id = d.plugin.id;
    const Version& v = d.version;
    if (v.size > g_set.maxDownload) {
        FailJob(id, v.version, "the download is larger than [Store] MaxDownloadMB");
        return false;
    }
    if (!install::EnsureDirs(g_paths)) {
        FailJob(id, v.version, "cannot create Mods\\.store");
        return false;
    }
    SetJob("downloading", id, v.version, "", 0, v.size);
    install::Expect e;
    e.id = id;
    e.version = v.version;
    e.sha256 = v.sha256;
    e.kind = v.kind;
    e.filesystem = v.filesystem;
    e.unsafe = v.unsafe;
    e.size = v.size;
    e.unpackedSize = v.unpackedSize;
    const std::wstring part = g_paths.Dl() + L"\\" + W(id + "-" + v.version + ".zip.part");
    fetch::Options o = BaseOptions();
    o.progress = [&](uint64_t got, uint64_t) {
        const ULONGLONG now = GetTickCount64();
        {
            std::lock_guard lk(g_mx);
            g_job.bytes = got;
        }
        if (now - g_lastProgress >= 100 || got == v.size) {
            g_lastProgress = now;
            PublishState();
        }
    };
    std::string err;
    LOG_INFO("[store] downloading %s %s (%llu bytes) from %s", id.c_str(), v.version.c_str(),
             static_cast<unsigned long long>(v.size), d.url.c_str());
    if (!install::FetchVerified(d.url, part, e, v.size, o, &err)) {
        FailJob(id, v.version, err);
        return false;
    }
    LOG_INFO("[store] %s %s verified (sha256 %s)", id.c_str(), v.version.c_str(), v.sha256.c_str());
    SetJob("verifying", id, v.version, "", v.size, v.size);
    SetJob("installing", id, v.version, "");
    install::Staged staged;
    const bool staged_ok = install::Stage(g_paths, part, e, melange.empty() ? g_host->MelangeVersion() : melange, &staged, &err, &g_cancel);
    DeleteFileW(part.c_str());
    if (!staged_ok) {
        FailJob(id, v.version, err);
        return false;
    }
    {
        std::string gate;
        {
            std::lock_guard lk(g_mx);
            gate = g_gate;
        }
        if (!gate.empty()) {
            install::DeleteTree(staged.root);
            FailJob(id, v.version, gate);
            return false;
        }
    }
    const auto local = LocalMods();
    const auto me = local.find(id);
    const bool known = me != local.end();
    const LocalMod entry = known ? me->second : LocalMod{};
    const bool present = install::Exists(g_paths.mods + L"\\" + W(id));
    install::Pending op{"update", id, v.version, v.sha256, staged.rel, 0, false};
    {
        std::lock_guard lk(g_mx);
        op.serial = g_index ? g_index->serial : 0;
    }
    if (!present) {
        if (install::PlaceNew(g_paths, id, staged, &err) != install::Result::Done) {
            FailJob(id, v.version, err);
            return false;
        }
        Record(id, v);
        g_host->Placed(id, enable);
        LOG_INFO("[store] installed %s %s", id.c_str(), v.version.c_str());
        *message = "Installed " + d.plugin.name + " " + v.version;
        return true;
    }
    const bool contentSession = known && entry.sessionActive && (entry.contentRelevant || v.kind == "content");
    if (contentSession) {
        AddPending(op);
        LOG_INFO("[store] %s %s staged: applies at the next launch", id.c_str(), v.version.c_str());
        *message = d.plugin.name + " " + v.version + " applies at the next launch";
        return true;
    }
    const bool wasOn = known && !entry.contentRelevant && entry.sessionActive;
    if (wasOn) g_host->Unload(id);
    const install::Result r = install::Replace(g_paths, id, staged, &err);
    if (r == install::Result::Done) Record(id, v);
    g_host->Reload(id, wasOn);
    if (r == install::Result::Pending) {
        AddPending(op);
        LOG_INFO("[store] %s %s staged (%s): applies at the next launch", id.c_str(), v.version.c_str(), err.c_str());
        *message = d.plugin.name + " " + v.version + " applies at the next launch";
        return true;
    }
    if (r == install::Result::Failed) {
        FailJob(id, v.version, err);
        return false;
    }
    LOG_INFO("[store] updated %s to %s", id.c_str(), v.version.c_str());
    *message = "Updated " + d.plugin.name + " to " + v.version;
    return true;
}

bool DoAdd(const Task& t) {
    std::string message;
    for (size_t i = 0; i < t.steps.size(); ++i) {
        if (g_cancel.load()) {
            FailJob(t.id, "", "cancelled");
            return false;
        }
        const bool last = i + 1 == t.steps.size();
        if (!PlaceStep(t.steps[i], last ? t.enable : true, t.melange, &message)) return false;
    }
    SetJob("done", t.id, t.steps.empty() ? "" : t.steps.back().version.version, message);
    return true;
}

enum class Dropped { Now, NextLaunch, Failed };

Dropped DoDrop(const Task& t) {
    const std::string& id = t.id;
    SetJob("removing", id, "", "");
    const auto local = LocalMods();
    const auto me = local.find(id);
    const bool known = me != local.end();
    const LocalMod entry = known ? me->second : LocalMod{};
    std::string kind;
    {
        std::lock_guard lk(g_mx);
        const Plugin* p = g_index ? FindPlugin(*g_index, id) : nullptr;
        auto rec = g_db.mods.find(id);
        const Version* v = p && rec != g_db.mods.end() ? FindVersion(*p, rec->second.version) : nullptr;
        if (v) kind = v->kind;
    }
    const install::Pending op{"remove", id, "", "", "", 0, t.deleteData};
    bool genActive = false;
    if (!t.keepGenerated)
        for (const std::string& g : install::GeneratedBy(g_paths, id))
            if (auto it = local.find(g); it != local.end() && it->second.sessionActive) genActive = true;
    if ((known && entry.sessionActive && (entry.contentRelevant || kind == "content")) || genActive) {
        AddPending(op);
        LOG_INFO("[store] removing %s at the next launch", id.c_str());
        SetJob("done", id, "", "Removed at the next launch");
        return Dropped::NextLaunch;
    }
    if (known) g_host->Unload(id);
    std::string err;
    const install::Result r = install::Remove(g_paths, id, &err);
    if (r == install::Result::Failed) {
        FailJob(id, "", err);
        return Dropped::Failed;
    }
    if (r == install::Result::Pending) {
        AddPending(op);
        SetJob("done", id, "", "Removed at the next launch");
        return Dropped::NextLaunch;
    }
    {
        std::lock_guard lk(g_mx);
        g_db.mods.erase(id);
        install::SaveDb(g_paths, g_db);
    }
    g_host->Forget(id);
    if (t.deleteData) DeleteData(id);
    if (!t.keepGenerated) {
        for (const std::string& g : install::GeneratedBy(g_paths, id)) g_host->Unload(g);
        for (const std::string& g : install::RemoveGenerated(g_paths, id, t.deleteData))
            LOG_INFO("[store] removed %s with %s", g.c_str(), id.c_str());
    }
    LOG_INFO("[store] removed %s", id.c_str());
    SetJob("done", id, "", "Removed");
    return Dropped::Now;
}

// Caller holds g_mx. The download steps for `id` at `target`, dependencies first; Install's checks.
std::string BuildStepsLocked(const Index& idx, const std::string& id, const std::string& target,
                             const std::map<std::string, LocalMod>& local, const Env& env, Task* t) {
    std::map<std::string, std::string> installed;
    for (const auto& [mid, l] : local) installed[mid] = l.version;
    installed.erase(id);
    std::vector<Step> plan;
    std::string why;
    if (!PlanInstall(idx, id, target, installed, env, &plan, &why)) return why;
    for (const Step& s : plan) {
        const Plugin* sp = FindPlugin(idx, s.id);
        const Version* sv = sp ? FindVersion(*sp, s.version) : nullptr;
        if (!sv) return s.id + " " + s.version + " is not in the list";
        if (s.id != id && local.count(s.id) && !g_db.mods.count(s.id)) return s.id + " was installed by hand; update it yourself first";
        StepData d;
        d.plugin = *sp;
        d.plugin.versions.clear();
        d.plugin.screenshots.clear();
        d.version = *sv;
        if (!ResolveUrl(g_set.indexUrl, sv->url, &d.url, &why)) return s.id + ": " + why;
        t->steps.push_back(std::move(d));
    }
    return {};
}

// The compatibility sweep's Store half: every finding, plus a Store plugin the list does not build for this game.
void DoReconcile(const Task& t) {
    if (t.fetch) DoFetch();   // falls back to the cached list when offline
    std::shared_ptr<const Index> idx;
    bool rollback, offline;
    {
        std::lock_guard lk(g_mx);
        idx = g_index;
        rollback = g_rollback;
        offline = g_offline;
    }
    if (!idx) {
        Index cached;
        std::string when;
        if (LoadCachedIndex(&cached, &when)) {
            idx = std::make_shared<const Index>(std::move(cached));
            std::lock_guard lk(g_mx);
            rollback = SchemeOf(g_set.indexUrl) != Scheme::File && idx->serial < g_db.serialSeen;
        }
    }
    const std::map<std::string, LocalMod> local = LocalMods();
    Env env = MakeEnv(rollback);
    if (!t.melange.empty()) env.melange = t.melange;
    const std::wstring modsDir = g_paths.mods;

    std::vector<compat::Finding> todo = t.found;
    if (idx && !env.gameBuild.empty()) {
        std::lock_guard lk(g_mx);
        for (const auto& [id, rec] : g_db.mods) {
            const Plugin* p = FindPlugin(*idx, id);
            const auto l = local.find(id);
            if (!p || l == local.end() || std::find(p->gameBuilds.begin(), p->gameBuilds.end(), env.gameBuild) != p->gameBuilds.end())
                continue;
            if (std::any_of(todo.begin(), todo.end(), [&](const compat::Finding& f) { return f.id == id; })) continue;
            todo.push_back({id, p->name, l->second.version, "not made for game build " + env.gameBuild});
        }
    }
    if (todo.empty()) return;
    std::string gate;
    {
        std::lock_guard lk(g_mx);
        gate = g_gate;   // the host's gate as Tick last saw it (the host is not asked off its own thread)
    }
    const std::string why = !gate.empty()             ? gate
                            : !idx                     ? std::string("no store list yet")
                            : env.gameBuild.empty()    ? std::string("the game build is not recognised")
                            : rollback                 ? std::string(kRollbackText)
                                                       : std::string();
    if (!why.empty()) {
        for (const compat::Finding& f : todo)
            LOG_WARN("[store] %s cannot load (%s); left in place: %s", f.id.c_str(), f.reason.c_str(), why.c_str());
        return;
    }

    for (const compat::Finding& f : todo) {
        {
            std::lock_guard lk(g_mx);
            if (!g_db.mods.count(f.id) || PendingFor(f.id)) continue;
        }
        compat::Notice n;
        n.id = f.id;
        n.name = f.name.empty() ? f.id : f.name;
        n.version = f.version;
        n.reason = f.reason;
        n.melange = env.melange;
        const Plugin* p = FindPlugin(*idx, f.id);
        if (p && !p->name.empty()) n.name = p->name;
        const Version* offer = nullptr;
        if (p)
            for (const Version& v : p->versions)
                if (!v.yanked && Compatible(v, *p, env, nullptr)) {
                    offer = &v;
                    break;
                }
        if (offer && offer->version == f.version) offer = nullptr;   // the same version cannot load either
        if (offer) {
            if (!t.fetch || offline) {
                LOG_INFO("[store] %s cannot load (%s); version %s can: update it from the Store", f.id.c_str(), f.reason.c_str(),
                         offer->version.c_str());
                continue;
            }
            Task add{Task::Add, f.id, {}, true, false};
            add.melange = t.melange;
            std::string err;
            {
                std::lock_guard lk(g_mx);
                err = BuildStepsLocked(*idx, f.id, offer->version, local, env, &add);
            }
            LOG_INFO("[store] %s cannot load (%s): updating it to %s", f.id.c_str(), f.reason.c_str(), offer->version.c_str());
            if (err.empty() && DoAdd(add)) {
                n.action = "updated";
                n.detail = offer->version;
            } else {
                if (err.empty()) {
                    std::lock_guard lk(g_mx);
                    err = g_job.message;
                }
                LOG_WARN("[store] %s: the update to %s failed: %s", f.id.c_str(), offer->version.c_str(), err.c_str());
                n.action = "failed";
                n.detail = "updating it to " + offer->version + " failed: " + err;
            }
        } else {
            // A cached list can predate the release that fixes this plugin (a Melange update usually lands first),
            // so removal waits for a list fetched just now; until then the plugin is only not loaded.
            if (!t.fetch || offline) {
                LOG_INFO("[store] %s cannot load (%s); left in place until a fresh Store list says no version can", f.id.c_str(),
                         f.reason.c_str());
                continue;
            }
            LOG_INFO("[store] %s cannot load (%s) and the list has no version that can: removing it", f.id.c_str(), f.reason.c_str());
            Task drop{Task::Drop, f.id, {}, true, false};
            drop.keepGenerated = true;
            const Dropped d = DoDrop(drop);
            if (d == Dropped::Failed) {
                std::lock_guard lk(g_mx);
                n.action = "failed";
                n.detail = "removing it failed: " + g_job.message;
            } else {
                n.action = "removed";
                if (d == Dropped::NextLaunch) n.detail = "at the next launch";
            }
        }
        compat::AddNotice(modsDir, n);
    }
}

void Worker() {
    for (;;) {
        Task t;
        {
            std::unique_lock lk(g_mx);
            g_cv.wait(lk, [] { return g_stop || !g_tasks.empty(); });
            if (g_stop) return;
            t = std::move(g_tasks.front());
            g_tasks.pop_front();
        }
        switch (t.kind) {
            case Task::Fetch: DoFetch(); break;
            case Task::Shots: DoShots(t.id); break;
            case Task::Add:
            case Task::Drop:
            case Task::Reconcile:
                g_cancel = false;
                if (t.kind == Task::Add) DoAdd(t);
                else if (t.kind == Task::Drop) DoDrop(t);
                else DoReconcile(t);
                {
                    std::lock_guard lk(g_mx);
                    // A Reconcile can be queued behind a running job: stay busy until the last change ran.
                    g_busy = std::any_of(g_tasks.begin(), g_tasks.end(), [](const Task& q) { return q.kind >= Task::Add; });
                    g_updatesDirty = true;
                }
                PublishState();
                break;
        }
    }
}

// Caller holds g_mx.
void EnqueueLocked(Task t) {
    if (!g_workerStarted) {
        g_workerStarted = true;
        std::thread(&Worker).detach();
    }
    g_tasks.push_back(std::move(t));
    g_cv.notify_one();
}

// ---- items --------------------------------------------------------------------------------------------------
using Local = LocalMod;

// Caller holds g_mx.
Item MakeItem(const Plugin& p, const std::map<std::string, Local>& local, const Env& env) {
    Item it;
    it.id = p.id;
    it.name = p.name;
    it.description = p.description;
    it.licence = p.licence;
    it.authors = p.authors;
    it.categories = p.categories;
    Have have;
    auto l = local.find(p.id);
    if (l != local.end()) {
        have.present = true;
        have.version = l->second.version;
        auto rec = g_db.mods.find(p.id);
        have.managed = rec != g_db.mods.end() && rec->second.version == l->second.version;
        it.installedState = l->second.state;
        it.enabled = l->second.enabled;
    }
    have.pending = PendingFor(p.id);
    const Choice c = Choose(p, have, env);
    const Version* shown = c.compatible;
    for (const Version& v : p.versions)
        if (!shown && !v.yanked) shown = &v;
    if (!shown) shown = &p.versions.front();
    for (const Version& v : p.versions)
        if (!v.yanked) {
            it.latest = v.version;
            break;
        }
    it.compatible = c.compatible ? c.compatible->version : "";
    it.kind = shown->kind;
    it.unsafe = shown->unsafe;
    it.size = shown->size;
    it.installed = have.present;
    it.managed = have.managed;
    it.installedVersion = have.version;
    it.action = c.action;
    it.canRemove = c.canRemove;
    it.state = c.state;
    it.reason = c.reason;
    if (auto e = g_rowError.find(p.id); e != g_rowError.end()) it.error = e->second;
    return it;
}

void RecomputeUpdates() {
    std::map<std::string, Local> local = LocalMods();
    std::lock_guard lk(g_mx);
    g_updates.clear();
    g_updatesDirty = false;
    if (!g_index || !g_fetchedThisSession) return;
    const Env env = MakeEnv(g_rollback);
    for (const Plugin& p : g_index->plugins) {
        const Item it = MakeItem(p, local, env);
        if (it.action == Action::Update && it.managed) g_updates.insert(p.id);
    }
}

std::string CurrentGate() {
    const std::string gate = g_host ? g_host->Gate() : std::string("the Store is not ready");
    std::lock_guard lk(g_mx);
    g_gate = gate;
    return gate;
}
}  // namespace

void SetHost(Host* h, const Config& c) {
    std::lock_guard lk(g_mx);
    const std::string before = g_set.indexUrl;
    g_host = h;
    g_set = c;
    if (g_set.indexUrl.empty()) g_set.indexUrl = kDefaultIndex;
    if (g_set.indexUrl != before) {
        // Another list (Melange.exe switched to a game folder with its own [Store] IndexUrl): the old one says nothing
        // about this one's plugins.
        g_index.reset();
        g_indexText.clear();
        g_fetchedAt.clear();
        g_fetchError.clear();
        g_offline = g_rollback = g_fetchedThisSession = g_fetchRequested = false;
        g_updatesDirty = true;
    }
}

bool Open(const std::wstring& modsDir, std::vector<std::string>* droppedIds) {
    {
        std::lock_guard lk(g_mx);
        if (g_busy) return false;
        g_paths = install::MakePaths(modsDir);
        g_db = install::Db{};
        g_pending.clear();
        g_rowError.clear();
        g_updatesDirty = true;
    }
    if (install::Exists(g_paths.root)) {
        install::Db db;
        install::LoadDb(g_paths, &db);
        if (droppedIds) {
            for (const install::Applied& a : install::ApplyPending(g_paths, &db)) {
                if (a.ok) LOG_INFO("[store] %s", a.message.c_str());
                else LOG_WARN("[store] deferred %s of %s: %s", a.op.op.c_str(), a.op.id.c_str(), a.message.c_str());
                {
                    std::lock_guard lk(g_mx);
                    g_notices.push_back(a.ok ? a.message : "Deferred " + a.op.op + " of " + a.op.id + " failed: " + a.message);
                }
                if (a.ok && a.op.op == "remove") {
                    droppedIds->push_back(a.op.id);
                    if (a.op.deleteData) DeleteData(a.op.id);
                }
            }
        }
        install::CleanLeftovers(g_paths);
        std::vector<install::Pending> pending;
        install::LoadPending(g_paths, &pending);
        std::lock_guard lk(g_mx);
        g_db = std::move(db);
        g_pending = std::move(pending);
    }
    g_active = g_host != nullptr;
    PublishState();
    return true;
}

void Close() {
    std::lock_guard lk(g_mx);
    if (g_busy) return;
    g_active = false;
    g_paths = install::Paths{};
    g_db = install::Db{};
    g_pending.clear();
}

void Tick() {
    if (!g_active.load()) return;
    const std::string gate = g_host->Gate();
    bool changed, dirty;
    {
        std::lock_guard lk(g_mx);
        changed = gate != g_gate;
        if (changed && !g_gate.empty()) {
            if (g_job.phase == "error" && g_job.message == g_gate) g_job = Job{};
            std::erase_if(g_rowError, [&](const auto& e) { return e.second == g_gate; });
        }
        g_gate = gate;
        dirty = g_updatesDirty;
    }
    if (changed) PublishState();
    if (dirty) RecomputeUpdates();
}

void MarkDirty() {
    std::lock_guard lk(g_mx);
    g_updatesDirty = true;
}

void Shutdown() {
    fetch::CancelActive();
    g_cancel = true;
    std::lock_guard lk(g_mx);
    g_stop = true;
    g_cv.notify_all();
}

std::string IndexText() {
    std::lock_guard lk(g_mx);
    return g_indexText;
}

bool Active() { return g_active.load(); }

Status GetStatus() {
    std::lock_guard lk(g_mx);
    Status s;
    s.enabled = g_active;
    s.indexUrl = g_set.indexUrl;
    s.customIndex = g_set.custom;
    s.fetchedAt = g_fetchedAt;
    s.offline = g_offline;
    s.fetching = g_fetching;
    s.haveIndex = g_index != nullptr;
    s.rollback = g_rollback;
    s.busy = g_busy;
    s.serial = g_index ? g_index->serial : -1;
    s.plugins = g_index ? g_index->plugins.size() : 0;
    s.error = g_fetchError;
    s.job = g_job;
    s.gate = g_gate;
    for (const install::Pending& p : g_pending) s.pending.push_back(p.id);
    s.notices = g_notices;
    return s;
}

std::vector<Item> List(const ListQuery& q) {
    std::vector<Item> out;
    std::map<std::string, Local> local = LocalMods();
    std::lock_guard lk(g_mx);
    if (!g_index) return out;
    const Env env = MakeEnv(g_rollback);
    for (const Plugin& p : g_index->plugins) {
        if (!Matches(p, q.query)) continue;
        if (!q.category.empty() && std::find(p.categories.begin(), p.categories.end(), q.category) == p.categories.end()) continue;
        Item it = MakeItem(p, local, env);
        if (q.filter == "installed" && !it.installed) continue;
        if (q.filter == "updates" && it.action != Action::Update) continue;
        if (it.state == "incompatible" && !it.installed && !q.incompatible && !g_set.showIncompatible) continue;
        out.push_back(std::move(it));
    }
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Item& a, const Item& b) {
        const bool ua = a.action == Action::Update, ub = b.action == Action::Update;
        if (ua != ub) return ua;
        return lower(a.name) < lower(b.name);
    });
    return out;
}

bool GetDetails(const std::string& id, Details* out, bool fetchShots) {
    std::map<std::string, Local> local = LocalMods();
    std::lock_guard lk(g_mx);
    const Plugin* p = g_index ? FindPlugin(*g_index, id) : nullptr;
    if (!p) return false;
    const Env env = MakeEnv(g_rollback);
    *out = Details{};
    out->item = MakeItem(*p, local, env);
    out->homepage = p->homepage;
    const Version* v = out->item.compatible.empty() ? &p->versions.front() : FindVersion(*p, out->item.compatible);
    out->filesystem = v->filesystem;
    out->content = v->kind == "content";
    out->dependencies = v->dependencies;
    out->conflicts = v->conflicts;
    out->imports = p->imports;
    for (const std::string& g : install::GeneratedBy(g_paths, id)) {
        spice::Manifest m;
        std::vector<spice::Error> errs;
        if (spice::Parse(g_paths.mods + L"\\" + W(g), &m, &errs)) out->importedMaps += static_cast<int>(m.levels.size());
    }
    bool missing = false;
    for (size_t i = 0; i < p->screenshots.size(); ++i) {
        ShotRow r;
        r.n = static_cast<int>(i + 1);
        r.caption = p->screenshots[i].caption;
        const std::wstring path = g_paths.Cache() + L"\\" + W(p->screenshots[i].sha256);
        r.ready = install::Exists(path + L".png") || install::Exists(path + L".jpg");
        missing |= !r.ready;
        out->screenshots.push_back(r);
    }
    for (const Version& x : p->versions) {
        VersionRow r;
        r.version = x.version;
        r.released = x.released;
        r.melange = x.melange;
        r.changelog = x.changelog;
        r.size = x.size;
        r.yanked = x.yanked;
        r.compatible = !x.yanked && Compatible(x, *p, env, nullptr);
        out->versions.push_back(r);
    }
    for (const auto& [mid, e] : local) {
        if (mid == id || !e.enabled) continue;
        for (const std::string& d : e.dependencies)
            if (d == id) out->dependants.push_back(mid);
        bool clash = false;
        for (const std::string& d : e.conflicts) clash |= d == id;
        for (const Dep& d : v->conflicts) clash |= d.id == mid;
        if (clash) out->conflictsEnabled.push_back(mid);
    }
    if (!out->item.compatible.empty()) {
        std::map<std::string, std::string> installed;
        for (const auto& [mid, l] : local) installed[mid] = l.version;
        installed.erase(id);
        if (!PlanInstall(*g_index, id, out->item.compatible, installed, env, &out->plan, &out->planError)) out->plan.clear();
    }
    if (fetchShots && missing && g_shotRequested.insert(id).second) EnqueueLocked(Task{Task::Shots, id, {}, true, false});
    return true;
}

Outcome Refresh() {
    std::lock_guard lk(g_mx);
    if (!g_active) return {-32000, "the Store is disabled"};
    g_fetchRequested = true;
    for (const Task& t : g_tasks)
        if (t.kind == Task::Fetch) return {};
    if (g_fetching) return {};
    EnqueueLocked(Task{Task::Fetch, "", {}, true, false});
    return {};
}

void EnsureFetched() {
    bool need;
    {
        std::lock_guard lk(g_mx);
        need = !g_fetchRequested;
    }
    if (need) Refresh();
}

Outcome Install(const std::string& id, const std::string& version, bool enable, bool replaceManual) {
    std::map<std::string, Local> local = LocalMods();
    const std::string gate = CurrentGate();
    std::lock_guard lk(g_mx);
    if (!g_active) return {-32000, "the Store is disabled"};
    if (!g_index) return {-32000, "the store list has not been fetched"};
    if (g_busy) return {-32002, "another install, update or remove is running"};
    if (!gate.empty()) return {-32000, gate};
    if (g_rollback) return {-32000, kRollbackText};
    const Plugin* p = FindPlugin(*g_index, id);
    if (!p) return {-32602, "no plugin '" + id + "' in the list"};
    if (PendingFor(id)) return {-32000, "a change to " + id + " is waiting for the next launch"};
    if (const std::string r = install::ReservedGeneratedId(g_paths, id); !r.empty()) return {-32000, r};
    const Env env = MakeEnv(false);
    const Item it = MakeItem(*p, local, env);
    const std::string target = version.empty() ? it.compatible : version;
    const Version* v = FindVersion(*p, target);
    if (target.empty() || !v) return {-32000, it.reason.empty() ? "no compatible version" : it.reason};
    std::string why;
    if (v->yanked) return {-32000, "version " + target + " was withdrawn"};
    if (!Compatible(*v, *p, env, &why)) return {-32000, why};
    if (it.installed && !it.managed && !replaceManual)
        return {-32000, "Mods\\" + id + " was installed by hand; confirm to replace it"};
    if (it.installed && it.managed && it.installedVersion == target) return {-32000, id + " " + target + " is already installed"};
    Task t{Task::Add, id, {}, enable, false};
    if (why = BuildStepsLocked(*g_index, id, target, local, env, &t); !why.empty()) return {-32000, why};
    for (const StepData& s : t.steps) g_rowError.erase(s.plugin.id);
    g_busy = true;
    g_job = Job{"downloading", id, target, "", 0, v->size};
    EnqueueLocked(std::move(t));
    return {};
}

Outcome Update(const std::string& id) {
    bool managed = false;
    {
        std::lock_guard lk(g_mx);
        managed = g_db.mods.count(id) != 0;
    }
    if (!managed) return {-32000, id + " was not installed from the Store"};
    return Install(id, "", true, false);
}

Outcome Remove(const std::string& id, bool deleteData) {
    std::map<std::string, Local> local = LocalMods();
    const std::string gate = CurrentGate();
    std::lock_guard lk(g_mx);
    if (!g_active) return {-32000, "the Store is disabled"};
    if (g_busy) return {-32002, "another install, update or remove is running"};
    if (!gate.empty()) return {-32000, gate};
    if (!g_db.mods.count(id) || !local.count(id)) return {-32000, id + " was not installed from the Store"};
    if (PendingFor(id)) return {-32000, "a change to " + id + " is waiting for the next launch"};
    g_rowError.erase(id);
    g_busy = true;
    g_job = Job{"removing", id, "", "", 0, 0};
    EnqueueLocked(Task{Task::Drop, id, {}, true, deleteData});
    return {};
}

bool Reconcile(const std::vector<compat::Finding>& found, bool fetch, const std::string& melange) {
    std::lock_guard lk(g_mx);
    if (!g_active || !g_host) return false;
    Task t{Task::Reconcile, "", {}, true, false};
    t.found = found;
    t.fetch = fetch && !found.empty();
    if (t.fetch) g_fetchRequested = true;
    t.melange = melange;
    g_busy = true;
    EnqueueLocked(std::move(t));
    return true;
}

bool Cancel() {
    {
        std::lock_guard lk(g_mx);
        if (!g_busy || g_job.phase != "downloading") return false;
    }
    g_cancel = true;
    fetch::CancelActive();
    return true;
}

bool OpenHomepage(const std::string& id, std::string* err) {
    std::string url;
    {
        std::lock_guard lk(g_mx);
        const Plugin* p = g_index ? FindPlugin(*g_index, id) : nullptr;
        if (!p) {
            *err = "no plugin '" + id + "' in the list";
            return false;
        }
        url = p->homepage;
    }
    if (SchemeOf(url) != Scheme::Https) {
        *err = "the plugin has no https:// homepage";
        return false;
    }
    ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

std::wstring ShotPath(const std::string& id, int n) {
    std::wstring base;
    {
        std::lock_guard lk(g_mx);
        const Plugin* p = g_index ? FindPlugin(*g_index, id) : nullptr;
        if (!p || n < 1 || n > static_cast<int>(p->screenshots.size())) return {};
        base = g_paths.Cache() + L"\\" + W(p->screenshots[static_cast<size_t>(n - 1)].sha256);
    }
    for (const wchar_t* ext : {L".png", L".jpg"})
        if (install::Exists(base + ext)) return base + ext;
    return {};
}

bool UpdateAvailable(const std::string& id) {
    std::lock_guard lk(g_mx);
    return g_updates.count(id) != 0;
}

std::string ChannelJson() {
    std::lock_guard lk(g_mx);
    jsonmini::Arr pending;
    for (const install::Pending& p : g_pending) pending.Str(p.id);
    jsonmini::Obj o;
    o.Str("phase", g_job.phase).Str("id", g_job.id).Str("version", g_job.version).UInt("bytes", g_job.bytes);
    o.UInt("total", g_job.total).Str("message", g_job.message).Raw("pending", pending.End()).Bool("fetching", g_fetching);
    o.Bool("busy", g_busy).Str("gate", g_gate).Int("serial", g_index ? g_index->serial : -1).Int("shots", g_shotsReady);
    if (!g_lastShotId.empty()) o.Raw("shot", jsonmini::Obj().Str("id", g_lastShotId).Int("n", g_lastShotN).End());
    return o.End();
}

}  // namespace melange::store
