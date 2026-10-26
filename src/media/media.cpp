// The media host: module [Media], players, resolves and torrents (host.h), the helper process (helper.h), the frame
// sections (shm.h) and their GL upload (upload_gl.h), and p:follow (media_sync.h). Main thread except where noted.
//
// The helper is started on demand (the first player, resolve or torrent) from a worker thread, because the launch
// check hashes about 100 MB (pack::VerifyForLaunch), and stopped after 20 s without users. Calls made while it starts
// only change the model here; when its hello arrives every torrent and player is replayed, which is also how a
// restart after a crash or a hang reopens each item where it was. At most one automatic restart a minute, then the
// helper is "dead" until a later open.
#include "media/media.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <type_traits>

#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/module.h"
#include "core/thread_guard.h"
#include "media/config_keys.h"
#include "media/helper.h"
#include "media/host.h"
#include "media/media_sync.h"
#include "media/pack.h"
#include "media/shm.h"
#include "media/upload_gl.h"
#include "media/url_logic.h"
#include "melange/jlog.h"
#include "net/net.h"
#include "net/peerbus.h"

namespace melange::media {
namespace {
using host::OpenSpec;

// ---------------------------------------------------------------- json payloads
json::Value JObj() {
    json::Value v;
    v.type = json::Type::Object;
    return v;
}
json::Value JArr() {
    json::Value v;
    v.type = json::Type::Array;
    return v;
}
json::Value JStr(const std::string& s) {
    json::Value v;
    v.type = json::Type::String;
    v.string = s;
    return v;
}
json::Value JNum(double n) {
    json::Value v;
    v.type = json::Type::Number;
    v.number = n;
    return v;
}
json::Value JBool(bool b) {
    json::Value v;
    v.type = json::Type::Bool;
    v.boolean = b;
    return v;
}
void JSet(json::Value& o, const char* k, json::Value v) { o.members.emplace_back(k, std::move(v)); }
json::Value TypeObj(const char* type) {
    json::Value o = JObj();
    JSet(o, "type", JStr(type));
    return o;
}

// ---------------------------------------------------------------- model
struct Player {
    uint32_t id = 0;
    std::string mod;
    uint32_t gen = 0;  // load generation: props of an earlier load are dropped
    // source
    std::string url;
    bool live = false;
    uint32_t torrent = 0;
    std::string ih;
    int file = -1;
    // what this player should be (sent on every (re)open)
    bool paused = true, mute = false;
    int volume = 70, quality = 720;
    double speed = 1;      // the mod's own speed (p:speed)
    double sentSpeed = 1;  // the speed last sent (the follow nudge changes it)
    std::string alang, slang;
    int aid = -1, sid = -1;  // -1: the default track, 0: off
    int sizeW = 0, sizeH = 0;
    double startAt = 0;  // where the next (re)open starts
    // what the helper reported for the current gen
    bool loaded = false, haveAudioPts = false;
    double sample = 0, sampleQpc = 0;
    bool repPaused = true, buffering = false, eof = false;
    double duration = 0, cacheTime = 0, repSpeed = 1;
    std::string title;
    int vw = 0, vh = 0;
    double aspect = 0;
    std::vector<pipe::Track> tracks;
    // follow
    sync::Follower follower;
    bool following = false;
    uint64_t hostId = 0;
    // frames
    std::unique_ptr<shmio::Section> section;
    gl::Uploader up;
    bool uploaded = false;
    int fw = 0, fh = 0;
    uint64_t drawnTick = 0;
};

struct Torrent {
    uint32_t id = 0;
    std::string mod, ih, magnet;
    uint32_t token = 0;
    bool haveFiles = false, delivered = false;
    std::string name;
    std::vector<pipe::BtFile> files;
    pipe::BtStatusEvt status;
    int selected = -1;
    std::set<int> touched;  // every file ever selected: the helper keeps a deselected file's data until the close
};

struct ResolveReq {
    uint32_t rid = 0;
    std::string mod, url;
    uint32_t token = 0;
    uint64_t deadline = 0;
};

enum class HState : uint8_t { Off, Starting, Ready, Restarting, Dead };
const char* HStateName(HState s) {
    switch (s) {
        case HState::Ready: return "ready";
        case HState::Restarting: return "restarting";
        case HState::Dead: return "dead";
        default: return "starting";
    }
}

// A launch on the worker thread: verify the pack, start the session. The main thread takes the result.
struct Launch {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string err;
    std::unique_ptr<helper::Session> session;
    pack::Held held;
};

bool g_installed = false;
bool g_osOk = true;
cfg::Media g_cfg;
host::Sink* g_sink = nullptr;
std::map<uint32_t, std::unique_ptr<Player>> g_players;
std::map<uint32_t, Torrent> g_torrents;
std::map<uint32_t, ResolveReq> g_resolves;
uint32_t g_nextPlayer = 1, g_nextTorrent = 1, g_nextRid = 1;
std::vector<gl::Uploader> g_glGraveyard;  // textures of closed players, freed in the next present
std::set<std::string> g_touched;          // mods that get "pack" events
std::string g_rand;                       // this game process's random part of every object name
std::wstring g_instanceDir;               // LocalLow\Melange\media-cache\<pid>-<rand>

HState g_hstate = HState::Off;
std::unique_ptr<helper::Session> g_session;
pack::Held g_held;
std::shared_ptr<Launch> g_launch;
helper::RestartPolicy g_restart;
bool g_swept = false;
bool g_btBound = false;  // a torrent was started outside a match (the firewall prompt is behind us)
uint64_t g_idleSince = 0;
bool g_lowHelper = true;

// pack events
pack::State g_packState = pack::State::Missing;
double g_packProgress = -1;
uint64_t g_packLast = 0;

// upload stats (jlog "media" every 10 s while uploading)
double g_upMsSum = 0, g_upMsMax = 0;
uint32_t g_upCount = 0, g_badFrames = 0;
uint64_t g_upReport = 0;

uint64_t Tick() { return GetTickCount64(); }

void Post(const std::string& mod, const json::Value& payload) {
    if (g_sink) g_sink->Event(mod, payload);
}

void PostHelperState() {
    std::set<std::string> mods;
    for (auto& [id, p] : g_players) mods.insert(p->mod);
    for (const std::string& m : mods) {
        json::Value o = TypeObj("helper");
        JSet(o, "state", JStr(HStateName(g_hstate)));
        Post(m, o);
    }
}

void SetHState(HState s) {
    if (g_hstate == s) return;
    g_hstate = s;
    LOG_INFO("[media] helper %s", s == HState::Off ? "off" : HStateName(s));
    if (s != HState::Off) PostHelperState();
}

bool HaveUsers() { return !g_players.empty() || !g_torrents.empty() || !g_resolves.empty(); }

std::wstring CacheRoot() {
    if (!g_cfg.cacheDir.empty()) return g_cfg.cacheDir;
    PWSTR p = nullptr;
    std::wstring root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &p)) && p) root = p;
    if (p) CoTaskMemFree(p);
    if (root.empty()) return {};
    return root + L"\\Melange\\media-cache";
}

std::string SafeHwdec(const std::string& s) {
    if (s.empty() || s.size() > 32) return "auto-copy";
    for (char c : s)
        if (!url::IdChar(c)) return "auto-copy";
    return s;
}

pipe::HelloCmd HelloFromConfig() {
    pipe::HelloCmd h;
    h.hwdec = SafeHwdec(g_cfg.hwdec);
    h.audioDelayMs = g_cfg.audioDelayMs;
    h.maxHeight = g_cfg.maxHeight;
    return h;
}

sync::Clock ClockFor(uint64_t hostId) {
    sync::Clock c;
    peerbus::ClockInfo ci;
    if (peerbus::Clock(hostId, &ci) && ci.samples >= 2) {
        c.ready = true;
        c.offsetMs = ci.offsetMs;
    }
    return c;
}

// ---------------------------------------------------------------- helper lifecycle
void StartLaunch(bool sweep) {
    g_cfg = cfg::ReadMedia();
    g_lowHelper = g_cfg.helperLowIntegrity;
    if (!g_lowHelper) LOG_WARN("[media] HelperLowIntegrity=0: the helper runs with the game's full rights");
    if (g_instanceDir.empty()) {
        const std::wstring root = CacheRoot();
        if (!root.empty())
            g_instanceDir = root + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(g_rand.begin(), g_rand.end());
    }
    auto job = std::make_shared<Launch>();
    g_launch = job;
    const std::wstring dir = g_instanceDir;
    const bool low = g_lowHelper;
    const pipe::HelloCmd hello = HelloFromConfig();
    std::thread([job, dir, low, hello, sweep] {
        GuardedThreadBody("media launch", [&] {
            pack::Paths paths;
            if (!pack::ReadyPaths(&paths)) {
                job->err = "the media pack is not ready";
            } else if (dir.empty()) {
                job->err = "no cache folder (LocalLow)";
            } else if (pack::VerifyForLaunch(paths, &job->held, &job->err) && helper::PrepareCacheDir(dir, &job->err)) {
                if (paths.customLibmpv) LOG_WARN("[media] loading a user-supplied libmpv-2.dll (AllowCustomLibmpv=1), unverified");
                helper::LaunchSpec spec;
                spec.exe = paths.helperExe;
                spec.cacheDir = dir;
                spec.lowIntegrity = low;
                spec.args = {proto::arg::kParent, std::to_wstring(GetCurrentProcessId()), proto::arg::kCache, dir,
                             proto::arg::kLibmpv, paths.libmpv, proto::arg::kYtdlp, paths.ytdlp, proto::arg::kDeno, paths.deno};
                if (sweep) spec.args.push_back(proto::arg::kSweep);
                auto s = std::make_unique<helper::Session>();
                if (s->Start(spec, hello, pipeio::Timings{}, &job->err)) {
                    job->session = std::move(s);
                    job->ok = true;
                }
            }
            if (!job->ok) pack::ReleaseHeld(&job->held);
        });
        job->done.store(true);
    }).detach();
}

void EnsureHelper() {
    if (g_hstate == HState::Starting || g_hstate == HState::Ready || g_hstate == HState::Restarting) return;
    SetHState(HState::Starting);
    StartLaunch(!g_swept);
    g_swept = true;
}

void SendNow(const pipe::Message& m) {
    if (g_hstate == HState::Ready && g_session) g_session->Send(m);
}

// The open message for a player's current load.
pipe::OpenCmd OpenMessage(const Player& p) {
    pipe::OpenCmd c;
    c.id = p.id;
    c.gen = p.gen;
    c.shm = p.section ? p.section->Name() : std::string();
    if (!p.url.empty()) {
        c.url = p.url;
    } else {
        c.ih = p.ih;
        c.file = p.file;
    }
    c.start = std::max(0.0, p.startAt);
    c.paused = p.paused;
    c.volume = p.volume;
    c.mute = p.mute;
    c.speed = p.sentSpeed;
    c.height = std::min(p.quality, g_cfg.maxHeight);
    c.alang = p.alang;
    c.slang = p.slang;
    return c;
}

void SendSet(const Player& p, const char* prop, bool isBool, bool b, double n) {
    pipe::SetCmd s;
    s.id = p.id;
    s.prop = prop;
    s.isBool = isBool;
    s.boolean = b;
    s.number = n;
    SendNow(s);
}

double PosNow(const Player& p) {
    const bool advancing = p.loaded && !p.repPaused && !p.buffering && !p.eof;
    return sync::Extrapolate(p.sample, p.sampleQpc, peerbus::NowMs(), p.repSpeed, advancing, p.duration);
}

// A new load of the player's item (open, quality, helper restart): a new gen, a fresh reader, the same settings.
void Reload(Player& p, double at) {
    ++p.gen;
    p.startAt = at;
    p.loaded = p.haveAudioPts = false;
    p.eof = false;
    p.buffering = false;
    p.sample = at;
    p.sampleQpc = 0;
    p.tracks.clear();
    if (p.section) p.section->Reader().Reset();
    p.follower.Reopened();
    if (g_hstate != HState::Ready) return;
    SendNow(OpenMessage(p));
    if (p.sizeW) {
        pipe::SizeCmd z;
        z.id = p.id;
        z.w = p.sizeW;
        z.h = p.sizeH;
        SendNow(z);
    }
    if (p.aid >= 0) SendSet(p, proto::setprop::kAid, p.aid == 0, false, p.aid);
    if (p.sid >= 0) SendSet(p, proto::setprop::kSid, p.sid == 0, false, p.sid);
}

void SendBtAdd(const Torrent& t) {
    pipe::BtAddCmd c;
    c.ih = t.ih;
    c.magnet = t.magnet;
    c.uploadKBps = g_cfg.torrentUploadKBps;
    c.upnp = g_cfg.torrentUpnp;
    c.port = g_cfg.torrentPort;
    SendNow(c);
}

// Hello received: replay every torrent, player and resolve (first start and restarts alike).
void Replay() {
    std::set<std::string> added;
    for (auto& [id, t] : g_torrents) {
        if (!added.insert(t.ih).second) continue;
        SendBtAdd(t);
    }
    for (auto& [id, t] : g_torrents)
        if (t.selected >= 0) {
            pipe::BtSelectCmd s;
            s.ih = t.ih;
            s.index = t.selected;
            SendNow(s);
        }
    const double now = peerbus::NowMs();
    for (auto& [id, p] : g_players) {
        const double at = p->following && p->follower.Active() ? p->follower.TargetPos(now, ClockFor(p->hostId)) : p->startAt;
        Reload(*p, at);
    }
    for (auto& [rid, r] : g_resolves) {
        pipe::ResolveCmd c;
        c.rid = rid;
        c.url = r.url;
        SendNow(c);
    }
}

void EndSession(const std::string& why) {
    if (g_session) g_session->Stop(500);
    g_session.reset();
    pack::ReleaseHeld(&g_held);
    LOG_WARN("[media] helper session ended: %s", why.c_str());
}

void ErrorToPlayers(const std::string& message) {
    for (auto& [id, p] : g_players) {
        json::Value o = TypeObj("error");
        JSet(o, "code", JStr("helper"));
        JSet(o, "message", JStr(message));
        Post(p->mod, o);
    }
}

void OnBroken(const std::string& why) {
    EndSession(why);
    if (!HaveUsers()) {
        SetHState(HState::Off);
        return;
    }
    // Where each player was, so the restart reopens it there (a follower re-targets the host by itself).
    for (auto& [id, p] : g_players) {
        p->startAt = PosNow(*p);
        p->loaded = false;
    }
    if (g_restart.Allow(static_cast<double>(Tick()))) {
        SetHState(HState::Restarting);
        StartLaunch(false);
    } else {
        SetHState(HState::Dead);
        ErrorToPlayers("the media helper stopped again less than a minute after a restart");
    }
}

// ---------------------------------------------------------------- helper -> core
Player* PlayerFor(uint32_t id, uint32_t gen) {
    auto it = g_players.find(id);
    if (it == g_players.end() || it->second->gen != gen) return nullptr;
    return it->second.get();
}

void OnProp(const pipe::PropEvt& e) {
    Player* p = PlayerFor(e.id, e.gen);
    if (!p) return;
    namespace pr = proto::prop;
    using K = pipe::PropEvt::Kind;
    const bool num = e.kind == K::Number;
    if (e.name == pr::kTimePos || e.name == pr::kAudioPts) {
        if (!num) return;
        if (e.name == pr::kAudioPts) p->haveAudioPts = true;
        else if (p->haveAudioPts) return;  // what is heard wins over what is shown (Y1)
        p->sample = e.number;
        p->sampleQpc = e.qpcMs;
    } else if (e.name == pr::kPause) {
        if (e.kind == K::Bool) p->repPaused = e.boolean;
    } else if (e.name == pr::kPausedForCache) {
        const bool on = e.kind == K::Bool && e.boolean;
        if (on != p->buffering) {
            p->buffering = on;
            json::Value o = TypeObj("buffering");
            JSet(o, "on", JBool(on));
            Post(p->mod, o);
        }
    } else if (e.name == pr::kDuration) {
        p->duration = num ? e.number : 0;
    } else if (e.name == pr::kCacheTime) {
        p->cacheTime = num ? e.number : 0;
    } else if (e.name == pr::kEofReached) {
        p->eof = e.kind == K::Bool && e.boolean;
    } else if (e.name == pr::kSpeed) {
        if (num) p->repSpeed = e.number;
    } else if (e.name == pr::kMediaTitle) {
        p->title = e.kind == K::String ? e.str : std::string();
    } else if (e.name == pr::kVideoParams) {
        if (e.kind == K::Video) {
            p->vw = e.video.w;
            p->vh = e.video.h;
            p->aspect = e.video.aspect > 0 ? e.video.aspect : (e.video.h > 0 ? static_cast<double>(e.video.w) / e.video.h : 0);
        }
    } else if (e.name == pr::kTrackList) {
        p->tracks = e.tracks;
        Post(p->mod, TypeObj("tracks"));
    }
}

void OnEvent(const pipe::EventEvt& e) {
    Player* p = PlayerFor(e.id, e.gen);
    if (!p) return;
    if (e.name == proto::event::kFileLoaded) {
        p->loaded = true;
        // The helper sets mpv's pause before the loadfile, so its change event (if mpv sends one at all: none when the
        // value did not change, as for a fresh player opened playing) carries the previous gen and is dropped above.
        // mpv applied what was last commanded, in order: start from that, later pause events correct it.
        p->repPaused = p->paused;
        json::Value o = TypeObj("loaded");
        JSet(o, "title", JStr(p->title));
        JSet(o, "duration", JNum(p->duration));
        Post(p->mod, o);
    } else if (e.name == proto::event::kEndFile) {
        json::Value o = TypeObj("ended");
        JSet(o, "reason", JStr(e.reason.empty() ? "eof" : e.reason));
        Post(p->mod, o);
    }
}

bool Playable(const pipe::BtFile& f) { return f.video && url::IsVideoFile(f.path) && !url::IsRefusedFile(f.path); }

json::Value FilesJson(const std::vector<pipe::BtFile>& files) {
    json::Value a = JArr();
    for (const pipe::BtFile& f : files) {
        json::Value o = JObj();
        JSet(o, "index", JNum(f.index));
        JSet(o, "path", JStr(f.path));
        JSet(o, "size", JNum(f.size));
        JSet(o, "video", JBool(Playable(f)));  // the helper's flag is advice; the S3 picker rules decide
        a.items.push_back(std::move(o));
    }
    return a;
}

void FinishResolve(const ResolveReq& r, const json::Value& arg) {
    if (g_sink) g_sink->Callback(r.mod, r.token, arg, true);
}

// The file list reached this torrent: its callback (once) and a "torrent" event.
void DeliverFiles(Torrent& t) {
    const json::Value files = FilesJson(t.files);
    if (!t.delivered && g_sink) {
        t.delivered = true;
        json::Value arg = JObj();
        JSet(arg, "files", files);
        JSet(arg, "name", JStr(t.name));
        g_sink->Callback(t.mod, t.token, arg, false);
    }
    json::Value o = TypeObj("torrent");
    JSet(o, "infohash", JStr(t.ih));
    JSet(o, "files", files);
    Post(t.mod, o);
}

void OnError(const pipe::ErrorEvt& e) {
    if (e.rid) {
        auto it = g_resolves.find(e.rid);
        if (it == g_resolves.end()) return;
        json::Value o = JObj();
        const bool known = e.code == proto::err::kTimeout || e.code == proto::err::kNetwork || e.code == proto::err::kPrivate;
        JSet(o, "error", JStr(known ? e.code : std::string(proto::err::kYtdlp)));
        if (!e.message.empty()) JSet(o, "message", JStr(e.message));
        const ResolveReq r = it->second;
        g_resolves.erase(it);
        FinishResolve(r, o);
        return;
    }
    if (!e.ih.empty()) {
        for (auto& [id, t] : g_torrents)
            if (t.ih == e.ih && !t.delivered && g_sink) {
                json::Value o = JObj();
                JSet(o, "error", JStr(e.code));
                t.delivered = true;
                g_sink->Callback(t.mod, t.token, o, false);
            }
        return;
    }
    if (e.id) {
        Player* p = PlayerFor(e.id, e.gen);
        if (!p) return;
        if (e.code == proto::err::kYtdlp) pack::CheckYtdlpLane(true);  // a YouTube change broke the load: fetch a fix now
        json::Value o = TypeObj("error");
        JSet(o, "code", JStr(e.code));
        JSet(o, "message", JStr(e.message));
        Post(p->mod, o);
        return;
    }
    LOG_WARN("[media] helper error %s: %s", e.code.c_str(), e.message.c_str());
}

void OnResolved(const pipe::ResolvedEvt& e) {
    auto it = g_resolves.find(e.rid);
    if (it == g_resolves.end()) return;
    const ResolveReq r = it->second;
    g_resolves.erase(it);
    json::Value o = JObj();
    if (!e.error.empty()) {
        JSet(o, "error", JStr(e.error));
        if (!e.message.empty()) JSet(o, "message", JStr(e.message));
        if (e.error == proto::err::kYtdlp) pack::CheckYtdlpLane(true);  // YouTube changed: look for a fixed yt-dlp now
    } else {
        json::Value items = JArr();
        for (const pipe::ResolveItem& i : e.items) {
            json::Value one = JObj();
            JSet(one, "url", JStr(i.url));
            JSet(one, "id", JStr(i.id));
            JSet(one, "title", JStr(i.title));
            JSet(one, "duration", JNum(i.duration));
            items.items.push_back(std::move(one));
        }
        JSet(o, "items", std::move(items));
        if (!e.title.empty()) JSet(o, "title", JStr(e.title));
    }
    FinishResolve(r, o);
}

void OnBtFiles(const pipe::BtFilesEvt& e) {
    for (auto& [id, t] : g_torrents) {
        if (t.ih != e.ih) continue;
        t.files = e.files;
        t.name = e.name;
        t.haveFiles = true;
        DeliverFiles(t);
    }
}

void OnBtStatus(const pipe::BtStatusEvt& e) {
    for (auto& [id, t] : g_torrents)
        if (t.ih == e.ih) t.status = e;
}

void Dispatch(std::vector<pipe::Message>& msgs) {
    for (pipe::Message& m : msgs) {
        if (auto* h = std::get_if<pipe::HelloEvt>(&m)) {
            if (h->proto != proto::kProto) {
                LOG_ERROR("[media] the helper speaks protocol %d, Melange %d: not used", h->proto, proto::kProto);
                EndSession("protocol mismatch");
                SetHState(HState::Dead);
                ErrorToPlayers("the media helper does not match this Melange");
                return;
            }
            LOG_INFO("[media] helper ready: mpv %s, ffmpeg %s, libtorrent %s, hwdec %s%s", h->mpv.c_str(), h->ffmpeg.c_str(),
                     h->libtorrent.c_str(), h->hwdec.c_str(), g_session && g_session->Low() ? ", low integrity" : "");
            SetHState(HState::Ready);
            Replay();
        } else if (auto* p = std::get_if<pipe::PropEvt>(&m)) {
            OnProp(*p);
        } else if (auto* ev = std::get_if<pipe::EventEvt>(&m)) {
            OnEvent(*ev);
        } else if (auto* er = std::get_if<pipe::ErrorEvt>(&m)) {
            OnError(*er);
        } else if (auto* r = std::get_if<pipe::ResolvedEvt>(&m)) {
            OnResolved(*r);
        } else if (auto* f = std::get_if<pipe::BtFilesEvt>(&m)) {
            OnBtFiles(*f);
        } else if (auto* s = std::get_if<pipe::BtStatusEvt>(&m)) {
            OnBtStatus(*s);
        }
    }
}

// ---------------------------------------------------------------- frame pump
void PumpPack() {
    if (g_touched.empty()) return;
    const pack::Info info = pack::Get();
    const bool changed = info.state != g_packState || std::fabs(info.progress - g_packProgress) >= 0.001;
    if (!changed) return;
    const uint64_t now = Tick();
    if (info.state == g_packState && now - g_packLast < 250) return;  // progress at most 4 times a second
    g_packState = info.state;
    g_packProgress = info.progress;
    g_packLast = now;
    for (const std::string& m : g_touched) {
        json::Value o = TypeObj("pack");
        JSet(o, "state", JStr(pack::StateName(info.state)));
        JSet(o, "progress", JNum(info.progress));
        Post(m, o);
    }
}

// The inbox drops props and events for a player id not handed out yet (pipe.h).
void PublishIdCeiling() {
    if (g_session && g_session->Conn()) g_session->Conn()->In().SetIdCeiling(g_nextPlayer - 1);
}

void PumpHelper() {
    if (g_launch && g_launch->done.load()) {
        std::shared_ptr<Launch> l = std::move(g_launch);
        if (l->ok) {
            g_session = std::move(l->session);
            g_held = l->held;
        } else {
            LOG_ERROR("[media] the helper did not start: %s", l->err.c_str());
            SetHState(HaveUsers() ? HState::Dead : HState::Off);
            ErrorToPlayers(l->err);
            for (auto& [rid, r] : g_resolves) {
                json::Value o = JObj();
                JSet(o, "error", JStr(proto::err::kYtdlp));
                JSet(o, "message", JStr("the media helper could not start"));
                FinishResolve(r, o);
            }
            g_resolves.clear();
        }
    }
    if (!g_session) return;
    PublishIdCeiling();
    std::vector<pipe::Message> msgs;
    g_session->Drain(&msgs);
    Dispatch(msgs);
    if (g_session && g_session->GetState() == helper::Session::State::Broken) OnBroken(g_session->BrokenReason());
    // Idle: no player, torrent or resolve for 20 s stops the helper (it starts again on demand).
    if (g_session && !HaveUsers()) {
        if (!g_idleSince) g_idleSince = Tick();
        if (Tick() - g_idleSince > 20000) {
            g_session->Stop();
            g_session.reset();
            pack::ReleaseHeld(&g_held);
            SetHState(HState::Off);
            g_idleSince = 0;
        }
    } else {
        g_idleSince = 0;
    }
}

void PumpResolves() {
    const uint64_t now = Tick();
    for (auto it = g_resolves.begin(); it != g_resolves.end();) {
        if (now >= it->second.deadline) {
            const ResolveReq r = it->second;
            it = g_resolves.erase(it);
            json::Value o = JObj();
            JSet(o, "error", JStr(proto::err::kTimeout));
            FinishResolve(r, o);
        } else {
            ++it;
        }
    }
}

void PumpFollow() {
    const double now = peerbus::NowMs();
    for (auto& [id, pp] : g_players) {
        Player& p = *pp;
        if (!p.following) continue;
        sync::Local l;
        l.loaded = p.loaded && g_hstate == HState::Ready;
        l.pos = PosNow(p);
        l.paused = p.paused;
        l.buffering = p.buffering;
        l.speed = p.sentSpeed;
        l.live = p.live;
        l.sampleQpcMs = p.sampleQpc;
        l.sampleValue = p.sample;
        const sync::Command cmd = p.follower.Step(now, l, ClockFor(p.hostId));
        if (cmd.setPause) {
            p.paused = cmd.pause;
            SendSet(p, proto::setprop::kPause, true, cmd.pause, 0);
        }
        if (cmd.setSpeed) {
            p.sentSpeed = cmd.speed;
            SendSet(p, proto::setprop::kSpeed, false, false, cmd.speed);
        }
        if (cmd.seek) {
            pipe::SeekCmd s;
            s.id = p.id;
            s.t = cmd.seekTo;
            s.mode = "exact";
            SendNow(s);
            p.sample = cmd.seekTo;
            p.sampleQpc = now;
            p.eof = false;
        }
    }
}

void OnFrame() {
    if (!g_installed) return;
    PumpPack();
    PumpHelper();
    PumpResolves();
    PumpFollow();
}

// ---------------------------------------------------------------- module
bool OsSupportsHelper() {
    using Wow2 = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto wow2 = reinterpret_cast<Wow2>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (wow2) {
        USHORT proc = 0, native = 0;
        if (wow2(GetCurrentProcess(), &proc, &native)) {
            if (native == IMAGE_FILE_MACHINE_ARM64) LOG_INFO("[media] ARM64 Windows: the x64 helper runs emulated");
            return native == IMAGE_FILE_MACHINE_AMD64 || native == IMAGE_FILE_MACHINE_ARM64;
        }
    }
    BOOL wow = FALSE;  // before Windows 10 1709 (T5): a 32-bit process under WOW64 means a 64-bit OS
    return IsWow64Process(GetCurrentProcess(), &wow) && wow;
}

class Media final : public Module {
public:
    const char* Name() const override { return "Media"; }
    const char* Description() const override { return "Watch-party media: the media pack, the helper process, video frames"; }
    int Order() const override { return 60; }  // after the Sandbox (50)
    bool Install() override {
        g_cfg = cfg::ReadMedia();  // documents the [Media] keys in a fresh ini
        g_osOk = OsSupportsHelper();
        g_rand = helper::RandomHex(8);
        if (!g_osOk) LOG_INFO("[media] 32-bit Windows: the 64-bit media helper cannot run (wum.media reports unsupported-os)");
        events::Subscribe(events::Event::Frame, [] { OnFrame(); });
        g_installed = true;
        return true;
    }
};
}  // namespace
MELANGE_MODULE(Media);

// ---------------------------------------------------------------- present hook and drawing
void OnPresent() {
    for (gl::Uploader& u : g_glGraveyard) u.Destroy();
    g_glGraveyard.clear();
    if (g_players.empty()) return;
    const uint64_t now = Tick();
    for (auto& [id, pp] : g_players) {
        Player& p = *pp;
        if (!p.section || !p.drawnTick || now - p.drawnTick > 250) continue;  // not drawn: decoding goes on, uploads stop
        shm::Frame f;
        const shm::ReadResult r = p.section->Reader().Acquire(&f);
        if (r == shm::ReadResult::Bad) {
            if (++g_badFrames <= 5) LOG_WARN("[media] player %u: a frame failed the checks and was skipped", p.id);
            continue;
        }
        if (r != shm::ReadResult::New) continue;
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        const bool ok = p.up.Upload(f.data, f.meta.w, f.meta.h);
        p.section->Reader().Release();
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        if (!ok) continue;
        p.uploaded = true;
        p.fw = static_cast<int>(f.meta.w);
        p.fh = static_cast<int>(f.meta.h);
        const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(fq.QuadPart);
        g_upMsSum += ms;
        g_upMsMax = std::max(g_upMsMax, ms);
        ++g_upCount;
    }
    if (g_upCount && now - g_upReport >= 10000) {
        jlog::Rec("media", jlog::Level::Debug, "upload_ms")
            .Float("avg", g_upMsSum / g_upCount)
            .Float("max", g_upMsMax)
            .Uint("frames", g_upCount)
            .Uint("bad", g_badFrames)
            .Emit();
        g_upMsSum = g_upMsMax = 0;
        g_upCount = 0;
        g_upReport = now;
    }
}

bool FrameForDraw(const char* modId, uint32_t playerId, DrawFrame* out) {
    auto it = g_players.find(playerId);
    if (!modId || it == g_players.end() || it->second->mod != modId) return false;
    Player& p = *it->second;
    p.drawnTick = Tick();
    if (!p.uploaded || !p.up.Texture()) return false;
    out->texture = p.up.Texture();
    out->w = p.fw;
    out->h = p.fh;
    out->u1 = static_cast<float>(p.fw) / static_cast<float>(shm::kMaxW);
    out->v1 = static_cast<float>(p.fh) / static_cast<float>(shm::kMaxH);
    out->aspect = p.aspect > 0 ? p.aspect : static_cast<double>(p.fw) / std::max(1, p.fh);
    return true;
}

// ---------------------------------------------------------------- host.h (wum.media)
namespace host {
namespace {
Player* Own(const std::string& mod, uint32_t id) {
    auto it = g_players.find(id);
    return it == g_players.end() || it->second->mod != mod ? nullptr : it->second.get();
}
Torrent* OwnTorrent(const std::string& mod, uint32_t id) {
    auto it = g_torrents.find(id);
    return it == g_torrents.end() || it->second.mod != mod ? nullptr : &it->second;
}
bool PackReady() {
    const pack::State s = pack::Get().state;
    return s == pack::State::Ready || s == pack::State::Updating;
}
int SnapHeight(int h) {
    int best = proto::kHeights[0];
    for (int x : proto::kHeights)
        if (x <= h) best = x;
    return best;
}
template <class T>
int CountOf(const std::map<uint32_t, T>& m, const std::string& mod) {
    int n = 0;
    for (const auto& [id, v] : m) {
        if constexpr (std::is_same_v<T, std::unique_ptr<Player>>) n += v->mod == mod;
        else n += v.mod == mod;
    }
    return n;
}
// The helper runs or is starting; false (reason "helper") if it died and was restarted less than a minute ago.
bool HelperUsable(std::string* reason) {
    if (g_hstate == HState::Dead) {
        if (!g_restart.Allow(static_cast<double>(Tick()))) {
            *reason = "helper";
            return false;
        }
        g_hstate = HState::Off;
    }
    EnsureHelper();
    return true;
}
bool FreeBytes(std::wstring dir, double* out) {
    while (!dir.empty() && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const size_t cut = dir.find_last_of(L'\\');
        dir = cut == std::wstring::npos ? std::wstring() : dir.substr(0, cut);
    }
    ULARGE_INTEGER b{};
    if (dir.empty() || !GetDiskFreeSpaceExW(dir.c_str(), &b, nullptr, nullptr)) return false;
    *out = static_cast<double>(b.QuadPart);
    return true;
}
}  // namespace

bool Available(std::string* reason) {
    if (!g_installed) {
        *reason = config::GetBool("Media", "Enabled", true) ? "unavailable" : "disabled";
        return false;
    }
    if (!g_osOk) {
        *reason = "unsupported-os";
        return false;
    }
    return true;
}

void Touch(const std::string& mod) { g_touched.insert(mod); }

uint32_t Open(const std::string& mod, const OpenSpec& s, std::string* reason) {
    if (!Available(reason)) return 0;
    if (CountOf(g_players, mod) >= proto::kMaxPlayersPerMod) {
        *reason = "busy";
        return 0;
    }
    if (!PackReady()) {
        *reason = "pack";
        return 0;
    }
    auto p = std::make_unique<Player>();
    Torrent* t = nullptr;
    if (!s.url.empty()) {
        url::Classified c;
        if (!url::Classify(s.url, &c) || c.kind != url::Kind::YouTube || c.yt.id.empty()) {
            *reason = "unsupported-url";
            return 0;
        }
        p->url = url::CanonicalVideoUrl(c.yt.id);  // a raw string never reaches the helper
        p->live = c.yt.live || s.live;
    } else {
        t = OwnTorrent(mod, s.torrent);
        if (!t) {
            *reason = "closed";
            return 0;
        }
        const pipe::BtFile* f = nullptr;
        for (const pipe::BtFile& x : t->files)
            if (x.index == s.file) f = &x;
        if (!t->haveFiles || !f || !Playable(*f)) {
            *reason = "unsupported-url";
            return 0;
        }
        // Every file any open torrent has ever selected (deselecting keeps the data on disk), plus this one,
        // must fit [Media] TorrentCacheMB, and this one the disk.
        double sum = 0;
        for (const auto& [id, o] : g_torrents)
            for (const pipe::BtFile& x : o.files)
                if (o.touched.count(x.index) || (id == t->id && x.index == s.file)) sum += x.size;
        double freeBytes = 0;
        const bool diskKnown = FreeBytes(g_instanceDir.empty() ? CacheRoot() : g_instanceDir, &freeBytes);
        if (sum > static_cast<double>(g_cfg.torrentCacheMB) * 1048576.0 || (diskKnown && f->size > freeBytes)) {
            *reason = "cache-cap";
            return 0;
        }
        p->torrent = t->id;
        p->ih = t->ih;
        p->file = s.file;
    }
    if (!HelperUsable(reason)) return 0;
    p->id = g_nextPlayer++;
    PublishIdCeiling();
    p->mod = mod;
    p->paused = s.paused;
    p->mute = s.mute;
    p->volume = std::clamp(s.volume, 0, 100);
    p->quality = SnapHeight(std::min(s.quality, g_cfg.maxHeight));
    p->speed = p->sentSpeed = std::clamp(s.speed, 0.25, 4.0);
    p->alang = url::ValidLang(s.alang) ? s.alang : std::string();
    p->slang = url::ValidLang(s.slang) ? s.slang : std::string();
    p->section = std::make_unique<shmio::Section>();
    std::string err;
    if (!p->section->Create(shmio::SectionName(GetCurrentProcessId(), g_rand, p->id), g_lowHelper, &err)) {
        LOG_ERROR("[media] frame section: %s", err.c_str());
        *reason = "helper";
        return 0;
    }
    if (t) t->touched.insert(s.file);
    if (t && t->selected != s.file) {
        t->selected = s.file;
        pipe::BtSelectCmd sel;
        sel.ih = t->ih;
        sel.index = s.file;
        SendNow(sel);
    }
    if (!p->url.empty()) pack::CheckYtdlpLane(false);  // at most once a day ([Media] YtDlpUpdate)
    const uint32_t id = p->id;
    Player& ref = *p;
    g_players[id] = std::move(p);
    Reload(ref, std::max(0.0, s.start));
    return id;
}

bool SetPaused(const std::string& mod, uint32_t id, bool paused) {
    Player* p = Own(mod, id);
    if (!p) return false;
    if (p->following) return true;  // the followed host decides
    p->paused = paused;
    SendSet(*p, proto::setprop::kPause, true, paused, 0);
    return true;
}

bool Seek(const std::string& mod, uint32_t id, double t, const std::string& mode) {
    Player* p = Own(mod, id);
    if (!p) return false;
    double to = mode == "relative" ? PosNow(*p) + t : t;
    if (p->duration > 0) to = std::min(to, p->duration);
    to = std::max(0.0, to);
    pipe::SeekCmd s;
    s.id = id;
    s.t = mode == "relative" ? t : to;
    s.mode = mode;
    SendNow(s);
    p->sample = to;
    p->sampleQpc = peerbus::NowMs();
    p->eof = false;
    p->startAt = to;
    return true;
}

bool SetSpeed(const std::string& mod, uint32_t id, double speed) {
    Player* p = Own(mod, id);
    if (!p) return false;
    p->speed = std::clamp(speed, 0.25, 4.0);
    if (!p->following) {
        p->sentSpeed = p->speed;
        SendSet(*p, proto::setprop::kSpeed, false, false, p->speed);
    }
    return true;
}

bool SetVolume(const std::string& mod, uint32_t id, double volume) {
    Player* p = Own(mod, id);
    if (!p) return false;
    p->volume = static_cast<int>(std::lround(std::clamp(volume, 0.0, 100.0)));
    SendSet(*p, proto::setprop::kVolume, false, false, p->volume);
    return true;
}

bool SetMute(const std::string& mod, uint32_t id, bool mute) {
    Player* p = Own(mod, id);
    if (!p) return false;
    p->mute = mute;
    SendSet(*p, proto::setprop::kMute, true, mute, 0);
    return true;
}

bool SetTrack(const std::string& mod, uint32_t id, bool audio, int track) {
    Player* p = Own(mod, id);
    if (!p) return false;
    track = std::clamp(track, 0, 9999);
    (audio ? p->aid : p->sid) = track;
    SendSet(*p, audio ? proto::setprop::kAid : proto::setprop::kSid, track == 0, false, track);
    return true;
}

bool SetQuality(const std::string& mod, uint32_t id, int height) {
    Player* p = Own(mod, id);
    if (!p) return false;
    const int q = SnapHeight(std::min(height, g_cfg.maxHeight));
    if (q == p->quality) return true;
    p->quality = q;
    Reload(*p, PosNow(*p));  // loadfile replace at the current time, pause and speed kept
    return true;
}

bool SetSize(const std::string& mod, uint32_t id, int w, int h) {
    Player* p = Own(mod, id);
    if (!p) return false;
    // A few render targets only (the corner and panel sizes), so a resize animation does not reconfigure the decoder
    // every frame: steps of 64x36, at most 1280x720, at least 256x144.
    auto up = [](int v, int step, int lo, int hi) { return std::clamp((std::max(v, 1) + step - 1) / step * step, lo, hi); };
    const int qw = up(w, 64, 256, 1280), qh = up(h, 36, 144, 720);
    if (qw == p->sizeW && qh == p->sizeH) return true;
    p->sizeW = qw;
    p->sizeH = qh;
    pipe::SizeCmd z;
    z.id = id;
    z.w = qw;
    z.h = qh;
    SendNow(z);
    return true;
}

bool Follow(const std::string& mod, uint32_t id, const FollowState* st, uint64_t hostId) {
    Player* p = Own(mod, id);
    if (!p) return false;
    if (!st) {
        if (p->following) {
            p->following = false;
            p->follower.Reset();
            p->sentSpeed = p->speed;
            SendSet(*p, proto::setprop::kSpeed, false, false, p->speed);
        }
        return true;
    }
    if (p->following && p->hostId != hostId) p->follower.Reset();
    sync::Target t;
    t.e = st->e;
    t.i = st->i;
    t.play = st->play;
    t.p = std::max(0.0, st->p);
    t.r = st->r;
    t.a = st->a;
    p->follower.Update(t, peerbus::NowMs());
    p->following = true;
    p->hostId = hostId;
    return true;
}

bool State(const std::string& mod, uint32_t id, PlayerState* out) {
    Player* p = Own(mod, id);
    if (!p) return false;
    out->time = PosNow(*p);
    out->duration = p->duration;
    out->paused = p->loaded ? p->repPaused : p->paused;
    out->buffering = p->buffering || (!p->loaded && g_hstate != HState::Dead);
    out->cacheAhead = std::max(0.0, p->cacheTime - out->time);
    out->speed = p->loaded ? p->repSpeed : p->sentSpeed;
    out->title = p->title;
    out->eof = p->eof;
    out->live = p->live;
    out->w = p->vw;
    out->h = p->vh;
    out->aspect = p->aspect;
    out->audio.clear();
    out->sub.clear();
    for (const pipe::Track& t : p->tracks) {
        if (t.type != "audio" && t.type != "sub") continue;
        (t.type == "audio" ? out->audio : out->sub).push_back({t.id, t.lang, t.title, t.selected});
    }
    out->following = p->following;
    out->hasDrift = p->following && p->follower.HasDrift();
    out->drift = p->follower.Drift();
    out->syncAction = p->following ? sync::ActionName(p->follower.LastAction()) : "none";
    out->helper = HStateName(g_hstate);
    return true;
}

bool Close(const std::string& mod, uint32_t id) {
    auto it = g_players.find(id);
    if (it == g_players.end() || it->second->mod != mod) return false;
    pipe::CloseCmd c;
    c.id = id;
    SendNow(c);
    g_glGraveyard.push_back(it->second->up);
    g_players.erase(it);
    return true;
}

uint32_t Resolve(const std::string& mod, const std::string& canonicalUrl, uint32_t token, std::string* reason) {
    if (!Available(reason)) return 0;
    if (CountOf(g_resolves, mod) >= proto::kMaxResolvesPerMod) {
        *reason = "busy";
        return 0;
    }
    if (!PackReady()) {
        *reason = "pack";
        return 0;
    }
    url::Classified c;
    if (!url::Classify(canonicalUrl, &c) || c.kind != url::Kind::YouTube) {
        *reason = "unsupported-url";
        return 0;
    }
    if (!HelperUsable(reason)) return 0;
    ResolveReq r;
    r.rid = g_nextRid++;
    r.mod = mod;
    r.url = c.url;
    r.token = token;
    r.deadline = Tick() + proto::kResolveTimeoutMs;
    pipe::ResolveCmd cmd;
    cmd.rid = r.rid;
    cmd.url = r.url;
    SendNow(cmd);  // or at hello (Replay)
    g_resolves[r.rid] = r;
    return r.rid;
}

void CancelResolve(const std::string& mod, uint32_t rid) {
    auto it = g_resolves.find(rid);
    if (it != g_resolves.end() && it->second.mod == mod) g_resolves.erase(it);
}

uint32_t AddTorrent(const std::string& mod, const std::string& magnet, uint32_t token, bool inMatch, std::string* reason) {
    if (!Available(reason)) return 0;
    if (CountOf(g_torrents, mod) >= proto::kMaxTorrentsPerMod) {
        *reason = "busy";
        return 0;
    }
    if (!PackReady()) {
        *reason = "pack";
        return 0;
    }
    url::Classified c;
    if (!url::Classify(magnet, &c) || c.kind != url::Kind::Magnet) {
        *reason = "unsupported-url";
        return 0;
    }
    // The first listen socket may raise a Windows Firewall prompt, which can minimise the game: never in a match (S6).
    const bool match = inMatch || wum::Phase() == wum::SessionPhase::Match;
    if (match && !g_btBound) {
        *reason = "first-use-in-match";
        return 0;
    }
    if (!HelperUsable(reason)) return 0;
    if (!match) g_btBound = true;
    Torrent t;
    t.id = g_nextTorrent++;
    t.mod = mod;
    t.ih = c.bt.infohash;
    t.magnet = c.url;
    t.token = token;
    const Torrent* known = nullptr;
    for (const auto& [id, o] : g_torrents)
        if (o.ih == t.ih) known = &o;
    if (known && known->haveFiles) {  // another mod has the same torrent: its file list is known already
        t.files = known->files;
        t.name = known->name;
        t.haveFiles = true;
    }
    const bool shared = known != nullptr;
    const uint32_t tid = t.id;
    Torrent& ref = g_torrents[tid];
    ref = std::move(t);
    if (!shared) SendBtAdd(ref);
    if (ref.haveFiles) DeliverFiles(ref);
    return ref.id;
}

bool TorrentFiles(const std::string& mod, uint32_t id, std::vector<File>* out, bool* have) {
    Torrent* t = OwnTorrent(mod, id);
    if (!t) return false;
    *have = t->haveFiles;
    out->clear();
    for (const pipe::BtFile& f : t->files) out->push_back({f.index, f.path, f.size, Playable(f)});
    return true;
}

bool TorrentStatusOf(const std::string& mod, uint32_t id, TorrentStatus* out) {
    Torrent* t = OwnTorrent(mod, id);
    if (!t) return false;
    out->down = t->status.down;
    out->up = t->status.up;
    out->peers = t->status.peers;
    out->progress = t->status.progress;
    out->have = t->status.have;
    out->metadata = t->haveFiles;
    return true;
}

bool CloseTorrent(const std::string& mod, uint32_t id) {
    Torrent* t = OwnTorrent(mod, id);
    if (!t) return false;
    const std::string ih = t->ih;
    // A player streaming from it closes first (the helper closes the stream before it removes the torrent).
    for (auto it = g_players.begin(); it != g_players.end();) {
        if (it->second->torrent == id) {
            pipe::CloseCmd c;
            c.id = it->first;
            SendNow(c);
            g_glGraveyard.push_back(it->second->up);
            it = g_players.erase(it);
        } else {
            ++it;
        }
    }
    g_torrents.erase(id);
    bool still = false;
    for (const auto& [tid, o] : g_torrents) still |= o.ih == ih;
    if (!still) {
        pipe::BtRemoveCmd r;
        r.ih = ih;
        SendNow(r);
    }
    return true;
}

void SetSink(Sink* sink) { g_sink = sink; }
}  // namespace host
}  // namespace melange::media
