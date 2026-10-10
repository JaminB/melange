// import.*: local content importers (a plugin ships a recipe; Melange.exe downloads or copies the zip, verifies it and
// generates map packs under Mods\). Refused while the game runs. The "import" channel carries the job.
#include <windows.h>
#include <objbase.h>

#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "core/log.h"
#include "core/thread_guard.h"

#include "import/importer.h"
#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/setup/running.h"
#include "launcher/util.h"
#include "levels/hidden.h"
#include "oasis/core/http.h"
#include "oasis/core/server.h"
#include "oasis/standalone/mods_provider.h"
#include "store/fetch.h"
#include "store/store.h"
#include "tools/json_mini.h"
#include "version.h"

namespace melange::launcher::rpc {
namespace {
using oasis::Call;
using oasis::Result;
namespace imp = melange::import;
namespace oc = melange::oasis::core;

struct Job {
    std::string plugin, phase = "idle", reason, message;
    uint64_t bytes = 0, total = 0;
    int step = 0, of = 0;
    bool hasResult = false;
    imp::RunResult result;
};

std::mutex g_mx;
Job g_job;
bool g_running = false;
std::atomic<bool> g_cancel{false};
std::map<std::string, std::wstring> g_browsed;
oasis::ChannelId g_channel = 0;

bool ValidPlugin(const std::string& id) { return spice::ValidModId(id); }

void RefuseReason(Result& r, int code, const std::string& msg, const std::string& reason) {
    Fail(r, code, msg, jsonmini::Obj().Str("reason", reason).End());
}

// "" or the reason code why an import, re-import or uninstall is refused now.
std::string Gate(const std::wstring& game) {
    if (game.empty() || !DirExists(game)) return "noGame";
    if (setup::GameRunning(game)) return "gameRunning";
    return "";
}

std::string Arr(const std::vector<std::string>& v) {
    jsonmini::Arr a;
    for (const auto& s : v) a.Str(s);
    return a.End();
}

std::string CountsJson(const std::map<std::string, int>& c, int skipped) {
    jsonmini::Obj o;
    for (const auto& [k, v] : c) o.Int(k, v);
    o.Int("skipped", skipped);
    return o.End();
}

std::string JobJson(const Job& j) {
    jsonmini::Obj o;
    o.Str("plugin", j.plugin).Str("phase", j.phase).UInt("bytes", j.bytes).UInt("total", j.total).Int("step", j.step).Int("of", j.of);
    if (!j.reason.empty()) o.Str("reason", j.reason).Str("message", j.message);
    if (j.hasResult && j.result.ok) {
        const auto& r = j.result;
        o.Raw("result", jsonmini::Obj()
                            .Int("maps", r.maps)
                            .Raw("counts", CountsJson(r.counts, r.skipped))
                            .Raw("packs", Arr(r.packs))
                            .UInt("bytes", r.bytes)
                            .Str("fingerprint", r.fingerprint)
                            .Int("skipped", r.skipped)
                            .End());
    }
    return o.End();
}

std::map<std::string, bool> EnabledMap(const std::wstring& game) {
    std::map<std::string, bool> out;
    json::Value v;
    json::Error e;
    if (!json::Parse(oasis::standalone::modsprov::ListJson(game, MELANGE_VERSION), &v, &e) || !v.IsArray()) return out;
    for (const auto& m : v.items) {
        const json::Value* id = m.Get("id");
        const json::Value* st = m.Get("state");
        if (id && id->IsString()) out[id->string] = st && st->IsString() && st->string == "enabled";
    }
    return out;
}

std::string PacksJson(const std::wstring& game, const imp::State& st) {
    const auto enabled = EnabledMap(game);
    jsonmini::Arr a;
    for (const auto& p : st.packs) {
        spice::Manifest m;
        std::vector<spice::Error> errs;
        const bool ok = spice::Parse(game + L"\\Mods\\" + Widen(p.id), &m, &errs) && !m.implicit;
        auto it = enabled.find(p.id);
        a.Raw(jsonmini::Obj()
                  .Str("id", p.id)
                  .Str("name", ok ? m.name : p.id)
                  .Bool("enabled", it != enabled.end() && it->second)
                  .Int("levels", p.levels)
                  .Raw("category", Arr(p.categories))
                  .End());
    }
    return a.End();
}

bool ZipVerified(const std::wstring& path, const imp::Source& s) {
    return FileSize(path) == s.size && Sha256Cached(path) == s.sha256;
}

std::string ImporterJson(const std::wstring& game, const std::string& id) {
    imp::Plugin pl;
    std::string err;
    bool unsupported = false;
    const bool ok = imp::LoadPlugin(game, id, &pl, &err, &unsupported);
    const imp::Recipe& r = pl.recipe;
    const imp::Paths paths = imp::MakePaths(game, id);
    imp::State st;
    const bool have = ok && imp::LoadState(paths, &st);
    std::string reason;
    std::string status = !ok ? (unsupported ? "unsupported" : "damaged") : imp::Status(paths, pl, have ? &st : nullptr, &reason);
    if (!ok) reason = err;
    Job job;
    bool running = false;
    {
        std::lock_guard lk(g_mx);
        running = g_running && g_job.plugin == id;
        if (g_job.plugin == id) job = g_job;
    }
    if (running) status = "busy";
    jsonmini::Arr sources;
    for (const auto& s : r.sources)
        sources.Raw(jsonmini::Obj()
                        .Str("id", s.id)
                        .Str("name", s.name)
                        .Str("host", s.urls.empty() ? "" : imp::HostOf(s.urls.front()))
                        .Str("fileName", s.fileName)
                        .UInt("size", s.size)
                        .Str("sha256", s.sha256)
                        .End());
    jsonmini::Obj o;
    o.Str("plugin", id).Str("name", pl.name.empty() ? id : pl.name).Str("recipe", r.id).Str("recipeVersion", r.output.version);
    o.Int("format", r.format).Bool("supported", ok);
    o.Raw("content", jsonmini::Obj()
                         .Str("title", r.content.title)
                         .Str("publisher", r.content.publisher)
                         .Str("termsUrl", r.content.termsUrl)
                         .Str("credit", r.content.credit)
                         .End());
    o.Raw("sources", sources.End()).Raw("expect", jsonmini::Obj().Int("maps", r.select.expectMaps).End());
    o.Str("status", status);
    if (!reason.empty()) o.Str("statusReason", reason);
    if (have)
        o.Raw("imported", jsonmini::Obj()
                              .Str("recipeVersion", st.recipeVersion)
                              .Str("fingerprint", st.fingerprint)
                              .Str("importedAt", st.importedAt)
                              .Int("maps", st.maps)
                              .Raw("counts", CountsJson(st.counts, st.skipped))
                              .Raw("packs", PacksJson(game, st))
                              .UInt("bytes", st.bytes)
                              .End());
    if (!r.sources.empty()) {
        const std::wstring zip = paths.Dl() + L"\\" + Widen(r.sources.front().fileName);
        if (FileExists(zip)) o.Raw("zip", jsonmini::Obj().UInt("bytes", FileSize(zip)).Bool("verified", ZipVerified(zip, r.sources.front())).End());
    }
    o.Str("gate", Gate(game));
    if (!job.plugin.empty() && job.phase != "idle") o.Raw("job", JobJson(job));
    return o.End();
}

std::string ImportersJson() {
    const std::wstring game = app::GameDir();
    jsonmini::Arr a;
    if (!game.empty())
        for (const auto& id : imp::ImporterPlugins(game)) a.Raw(ImporterJson(game, id));
    return jsonmini::Obj().Raw("importers", a.End()).End();
}

void PublishImporters() {
    if (g_channel && oasis::HasSubscribers(g_channel)) oasis::Publish(g_channel, ImportersJson());
}

void PublishJob() {
    if (!g_channel || !oasis::HasSubscribers(g_channel)) return;
    std::string j;
    {
        std::lock_guard lk(g_mx);
        j = JobJson(g_job);
    }
    oasis::Publish(g_channel, "{\"job\":" + j + "}");
}

void OnSub(oasis::ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (subscribed) oasis::PublishTo(ch, client, ImportersJson());
}

bool PluginParam(const Call& c, Result& r, json::Value* p, std::string* id) {
    if (!Params(c, r, p)) return false;
    *id = Str(*p, "plugin");
    if (!ValidPlugin(*id)) {
        Fail(r, -32602, "expected {plugin}");
        return false;
    }
    return true;
}

bool Busy() {
    std::lock_guard lk(g_mx);
    return g_running;
}

// ---------------------------------------------------------------- methods
void List(const Call&, Result& r, void*) { r.json = ImportersJson(); }

void StatusM(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    const std::wstring game = app::GameDir();
    if (game.empty()) return RefuseReason(r, -32000, "Choose your game folder first.", "noGame");
    r.json = ImporterJson(game, id);
}

void Browse(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    std::wstring picked;
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        const COMDLG_FILTERSPEC filter[] = {{L"Zip files", L"*.zip"}};
        dlg->SetFileTypes(1, filter);
        dlg->SetTitle(L"Choose the zip to import");
        HWND owner = app::Window();
        if (owner) SetForegroundWindow(owner);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    picked = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    if (picked.empty()) {
        r.json = "{\"cancelled\":true}";
        return;
    }
    picked = FullPath(picked);
    {
        std::lock_guard lk(g_mx);
        g_browsed[id] = picked;
    }
    r.json = jsonmini::Obj().Str("path", Narrow(picked)).Str("name", Narrow(FileName(picked))).UInt("size", FileSize(picked)).End();
}

void Worker(imp::RunSpec spec) {
    auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    std::string lastPhase;
    spec.progress = [&](const imp::Progress& pr) {
        {
            std::lock_guard lk(g_mx);
            g_job.phase = pr.phase;
            g_job.bytes = pr.bytes;
            g_job.total = pr.total;
            g_job.step = pr.step;
            g_job.of = pr.of;
        }
        const auto now = std::chrono::steady_clock::now();
        if (pr.phase != lastPhase || now - last >= std::chrono::milliseconds(200)) {
            lastPhase = pr.phase;
            last = now;
            PublishJob();
        }
    };
    LOG_INFO("[import] %s: importing from %s", spec.plugin.id.c_str(),
             spec.kind == imp::RunSpec::Kind::File ? "a local zip" : ("source '" + spec.sourceId + "'").c_str());
    imp::RunResult res = imp::Run(spec);
    if (res.ok) LOG_INFO("[import] %s: %d maps in %zu packs, fingerprint %s", spec.plugin.id.c_str(), res.maps, res.packs.size(), res.fingerprint.c_str());
    else LOG_WARN("[import] %s: %s (%s)", spec.plugin.id.c_str(), res.reason.c_str(), res.message.c_str());
    {
        std::lock_guard lk(g_mx);
        g_job.hasResult = true;
        g_job.result = res;
        if (res.ok) {
            g_job.phase = "done";
        } else if (res.reason == "cancelled") {
            g_job.phase = "cancelled";
        } else {
            g_job.phase = "error";
            g_job.reason = res.reason;
            g_job.message = res.message;
        }
        g_running = false;
    }
    PublishJob();
    PublishImporters();
    app::PublishStatus();
}

void Start(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    const json::Value* src = p.Get("source");
    const json::Value* acc = p.Get("accepted");
    if (!src || !src->IsObject()) return Fail(r, -32602, "expected {plugin, source, keepZip, accepted}");
    if (!acc || !acc->IsBool() || !acc->boolean) return RefuseReason(r, -32000, "Tick the box to accept where the maps come from.", "notAccepted");
    const std::wstring game = app::GameDir();
    if (const std::string g = Gate(game); !g.empty())
        return RefuseReason(r, -32000, g == "noGame" ? "Choose your game folder first." : "Close Worms Ultimate Mayhem to import maps.", g);
    imp::RunSpec spec;
    spec.game = game;
    spec.keepZip = Bool(p, "keepZip", true);
    spec.cancel = &g_cancel;
    spec.userAgent = std::string("Melange/") + MELANGE_VERSION;
    std::string err;
    bool unsupported = false;
    if (!imp::LoadPlugin(game, id, &spec.plugin, &err, &unsupported))
        return RefuseReason(r, -32000, unsupported ? "This plugin needs a newer Melange." : err, unsupported ? "unsupported" : "badPath");
    const std::string kind = Str(*src, "kind");
    if (kind == "download") {
        spec.kind = imp::RunSpec::Kind::Download;
        spec.sourceId = Str(*src, "id");
        bool found = false;
        for (const auto& s : spec.plugin.recipe.sources) found |= s.id == spec.sourceId;
        if (!found) return Fail(r, -32602, "unknown source id");
    } else if (kind == "file") {
        spec.kind = imp::RunSpec::Kind::File;
        spec.file = Widen(Str(*src, "path"));
        std::wstring browsed;
        {
            std::lock_guard lk(g_mx);
            if (auto it = g_browsed.find(id); it != g_browsed.end()) browsed = it->second;
        }
        if (spec.file.empty() || browsed.empty() || PathKey(spec.file) != PathKey(browsed) || !FileExists(browsed))
            return RefuseReason(r, -32000, "Choose the zip with the Choose file button.", "badPath");
        if (spec.plugin.recipe.sources.empty()) return RefuseReason(r, -32000, "The recipe has no source.", "badPath");
        spec.file = browsed;
        spec.sourceId = spec.plugin.recipe.sources.front().id;
    } else {
        return Fail(r, -32602, "source.kind must be download or file");
    }
    if (store::GetStatus().busy) return Fail(r, -32002, "The Store is busy. Try again in a moment.");
    {
        std::lock_guard lk(g_mx);
        if (g_running) return Fail(r, -32002, "An import is already running.");
        g_running = true;
        g_cancel = false;
        g_job = Job{};
        g_job.plugin = id;
        g_job.phase = spec.kind == imp::RunSpec::Kind::File ? "copying" : "downloading";
    }
    std::thread([spec = std::move(spec)]() mutable {
        bool finished = false;
        GuardedThreadBody("import", [&] {
            Worker(std::move(spec));
            finished = true;
        });
        if (finished) return;
        {
            // Worker clears g_running itself; a failure before that must not leave the import stuck as running.
            std::lock_guard lk(g_mx);
            if (g_running) {
                g_running = false;
                g_job.phase = "error";
                g_job.reason = "internal";
                g_job.message = "The import stopped unexpectedly.";
            }
        }
        PublishJob();
    }).detach();
    PublishJob();
    r.json = "{\"started\":true}";
}

void Cancel(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    bool cancelling = false;
    {
        std::lock_guard lk(g_mx);
        cancelling = g_running && g_job.plugin == id;
    }
    if (cancelling) {
        g_cancel = true;
        store::fetch::CancelActive();
    }
    r.json = jsonmini::Obj().Bool("cancelling", cancelling).End();
}

void Maps(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    const std::wstring game = app::GameDir();
    const imp::Paths paths = imp::MakePaths(game, id);
    imp::State st;
    std::string text;
    std::vector<imp::MapInfo> maps;
    if (game.empty() || !imp::LoadState(paths, &st) || !ReadAll(paths.Catalogue(), &text, 4u << 20) || !imp::ParseCatalogue(text, &maps))
        return RefuseReason(r, -32000, "No maps were imported yet.", "notImported");
    const std::set<std::string> hidden(st.hiddenByFile.begin(), st.hiddenByFile.end());
    jsonmini::Arr a;
    for (const auto& m : maps) {
        jsonmini::Obj o;
        o.Str("file", m.file).Str("stem", m.stem).Str("pack", m.pack).Str("title", m.title);
        if (!m.author.empty()) o.Str("author", m.author);
        o.Str("group", m.group).Str("groupLabel", m.groupLabel).Str("category", m.category).Str("categoryLabel", m.categoryLabel);
        if (!m.mode.empty()) o.Str("mode", m.mode);
        o.Str("theme", m.theme).Str("timeOfDay", m.timeOfDay).Bool("survivor", m.survivor).Bool("hidden", hidden.count(m.file) != 0);
        o.Bool("preview", m.preview && FileExists(paths.Previews() + L"\\" + Widen(m.stem) + L".png"));
        a.Raw(o.End());
    }
    r.json = jsonmini::Obj().Raw("maps", a.End()).Raw("packs", PacksJson(game, st)).End();
}

void SetHiddenM(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    const json::Value* files = p.Get("files");
    const json::Value* hid = p.Get("hidden");
    if (!files || !files->IsArray() || files->items.size() > 1024 || !hid || !hid->IsBool())
        return Fail(r, -32602, "expected {plugin, files: [string], hidden: bool}");
    std::vector<std::string> list;
    for (const auto& f : files->items) {
        if (!f.IsString() || f.string.empty() || f.string.size() > 128) return Fail(r, -32602, "bad file name");
        list.push_back(f.string);
    }
    if (Busy()) return Fail(r, -32002, "An import is running.");
    std::string err;
    const int n = imp::SetHidden(imp::MakePaths(app::GameDir(), id), list, hid->boolean, &err);
    if (n < 0) return err == "notImported" ? RefuseReason(r, -32000, "No maps were imported yet.", "notImported")
                                           : Fail(r, -32000, "Could not save the hidden maps.");
    r.json = jsonmini::Obj().Int("hidden", n).End();
}

void SetPacks(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    const json::Value* packs = p.Get("packs");
    if (!packs || !packs->IsArray() || packs->items.size() > imp::kMaxPacks) return Fail(r, -32602, "expected {plugin, packs: [{id, enabled}]}");
    const std::wstring game = app::GameDir();
    if (const std::string g = Gate(game); !g.empty()) return RefuseReason(r, -32000, "Close Worms Ultimate Mayhem first.", g);
    if (Busy()) return Fail(r, -32002, "An import is running.");
    const imp::Paths paths = imp::MakePaths(game, id);
    imp::State st;
    if (!imp::LoadState(paths, &st)) return RefuseReason(r, -32000, "No maps were imported yet.", "notImported");
    for (const auto& e : packs->items) {
        const std::string pid = e.IsObject() ? Str(e, "id") : std::string();
        const json::Value* en = e.IsObject() ? e.Get("enabled") : nullptr;
        if (!en || !en->IsBool() || !imp::IsGeneratedBy(game + L"\\Mods\\" + Widen(pid), id))
            return Fail(r, -32602, "unknown pack '" + pid + "'");
    }
    for (const auto& e : packs->items)
        if (oasis::standalone::modsprov::SetEnabled(game, Str(e, "id"), e.Get("enabled")->boolean) < 0)
            return Fail(r, -32000, "Could not save the pack choice.");
    r.json = jsonmini::Obj().Raw("packs", PacksJson(game, st)).End();
    PublishImporters();
}

void UninstallM(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    const std::wstring game = app::GameDir();
    if (const std::string g = Gate(game); !g.empty())
        return RefuseReason(r, -32000, g == "noGame" ? "Choose your game folder first." : "Close Worms Ultimate Mayhem first.", g);
    if (Busy()) return Fail(r, -32002, "An import is running.");
    std::vector<std::string> removed;
    std::string err;
    if (!imp::Uninstall(imp::MakePaths(game, id), id, Bool(p, "deleteZip"), &removed, &err)) return Fail(r, -32000, err);
    {
        std::lock_guard lk(g_mx);
        if (g_job.plugin == id && !g_running) g_job = Job{};
    }
    r.json = jsonmini::Obj().Raw("removed", Arr(removed)).End();
    PublishJob();
    PublishImporters();
}

void DeleteZip(const Call& c, Result& r, void*) {
    json::Value p;
    std::string id;
    if (!PluginParam(c, r, &p, &id)) return;
    const std::wstring game = app::GameDir();
    if (const std::string g = Gate(game); !g.empty())
        return RefuseReason(r, -32000, g == "noGame" ? "Choose your game folder first." : "Close Worms Ultimate Mayhem first.", g);
    if (Busy()) return Fail(r, -32002, "An import is running.");
    const imp::Paths paths = imp::MakePaths(game, id);
    uint64_t freed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((paths.Dl() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring f = paths.Dl() + L"\\" + fd.cFileName;
            const uint64_t n = FileSize(f);
            if (DeleteFileW(f.c_str())) freed += n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    imp::State st;
    if (imp::LoadState(paths, &st)) {
        st.zipKept = false;
        imp::SaveState(paths, st);
    }
    r.json = jsonmini::Obj().UInt("freed", freed).End();
    PublishImporters();
}

// /import/previews/<plugin>/<stem>.png
bool RoutePreview(const oc::Request& rq, oc::Response* out, void*) {
    constexpr size_t kPrefix = 17;   // "/import/previews/"
    if (rq.path.size() <= kPrefix) return false;
    const std::string rest = rq.path.substr(kPrefix);
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return false;
    const std::string plugin = rest.substr(0, slash), file = rest.substr(slash + 1);
    if (!ValidPlugin(plugin) || file.size() < 5 || !file.ends_with(".png")) return false;
    const std::string stem = file.substr(0, file.size() - 4);
    if (levels::hidden::StemOfKey("Multi." + stem) != stem) return false;
    const std::wstring game = app::GameDir();
    if (game.empty()) return false;
    const std::wstring full = imp::MakePaths(game, plugin).Previews() + L"\\" + Widen(stem) + L".png";
    const DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    out->status = 200;
    out->contentType = "image/png";
    out->file = full;
    return true;
}
}  // namespace

bool ImportRunning() { return Busy(); }

void InstallImport() {
    using oasis::kRpcMutating;
    using oasis::kRpcServerThread;
    g_channel = oasis::AddChannel("import");
    oasis::OnSubscribe(g_channel, &OnSub, nullptr);
    oasis::AddMethod("import.list", &List, nullptr, kRpcServerThread);
    oasis::AddMethod("import.status", &StatusM, nullptr, kRpcServerThread);
    oasis::AddMethod("import.browse", &Browse, nullptr, oasis::kRpcNone);
    oasis::AddMethod("import.start", &Start, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("import.cancel", &Cancel, nullptr, kRpcServerThread);
    oasis::AddMethod("import.maps", &Maps, nullptr, kRpcServerThread);
    oasis::AddMethod("import.setHidden", &SetHiddenM, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("import.setPacks", &SetPacks, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("import.uninstall", &UninstallM, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("import.deleteZip", &DeleteZip, nullptr, kRpcServerThread | kRpcMutating);
    oc::AddRoute("/import/previews/", &RoutePreview, nullptr);
    if (const std::wstring game = app::GameDir(); Gate(game).empty())
        for (const auto& id : imp::ImporterPlugins(game)) imp::Recover(imp::MakePaths(game, id), id);
}
}  // namespace melange::launcher::rpc
