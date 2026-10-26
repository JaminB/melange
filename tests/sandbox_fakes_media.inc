// sandbox_selftest fakes for wum.media (owner: C4b): link-time stand-ins for media::host:: and media::pack::, so
// wum_media.cpp runs without the helper. Included inside namespace melange, after the other fakes. The fake host keeps
// a small model the cases inspect (fake_media::*) and delivers resolve/torrent results and events on request.
// (media::FrameForDraw is C5's fake, in sandbox_fakes_draw.inc.)
}  // namespace melange
#include "input/gesture.h"  // not among the test file's own includes (the cases mark clicks)
#include "media/host.h"
namespace melange {
namespace fake_media {
struct Player {
    std::string mod;
    media::host::OpenSpec spec;
    bool paused = true, mute = false, following = false;
    double speed = 1, volume = 70, lastSeek = -1;
    std::string lastSeekMode;
    int quality = 720, w = 0, h = 0, aid = -1, sid = -1;
    media::host::FollowState follow;
    uint64_t hostId = 0;
};
struct Torrent {
    std::string mod, magnet;
    uint32_t token = 0;
    bool haveFiles = false;
    std::vector<media::host::File> files;
};
struct Resolve {
    std::string mod, url;
    uint32_t token = 0;
};
inline bool g_available = true;
inline std::string g_reason = "unavailable";
inline media::pack::State g_packState = media::pack::State::Missing;
inline int g_installs = 0;
inline std::string g_installMod;
inline bool g_inMatchSeen = false;
inline std::map<uint32_t, Player> g_players;
inline std::map<uint32_t, Torrent> g_torrents;
inline std::map<uint32_t, Resolve> g_resolves;
inline uint32_t g_next = 1;
inline std::set<std::string> g_touched;
inline media::host::Sink* g_sink = nullptr;

inline json::Value Parse(const std::string& s) {
    json::Value v;
    json::Error e;
    json::Parse(s, &v, &e);
    return v;
}
// Delivers a resolve's answer to Lua (the callback runs now) and forgets it.
inline void AnswerResolve(uint32_t rid, const std::string& argJson) {
    auto it = g_resolves.find(rid);
    if (it == g_resolves.end() || !g_sink) return;
    const Resolve r = it->second;
    g_resolves.erase(it);
    g_sink->Callback(r.mod, r.token, Parse(argJson), true);
}
inline void TorrentFilesArrive(uint32_t id) {
    auto it = g_torrents.find(id);
    if (it == g_torrents.end() || !g_sink) return;
    it->second.haveFiles = true;
    it->second.files = {{0, "Movie/movie.mkv", 1e8, true}, {1, "Movie/info.txt", 10, false}};
    g_sink->Callback(it->second.mod, it->second.token,
                     Parse(R"({"name":"Movie","files":[{"index":0,"path":"Movie/movie.mkv","size":1e8,"video":true},)"
                           R"({"index":1,"path":"Movie/info.txt","size":10,"video":false}]})"),
                     false);
}
inline void Event(const std::string& mod, const std::string& json) {
    if (g_sink) g_sink->Event(mod, Parse(json));
}
}  // namespace fake_media

namespace media::pack {
const char* StateName(State s) {
    static const char* const kNames[] = {"missing", "downloading", "verifying", "installing", "ready",
                                         "updating", "error",       "unsupported", "disabled"};
    return kNames[static_cast<int>(s)];
}
Info Get() {
    Info i;
    i.state = fake_media::g_packState;
    i.version = "1";
    i.downloadMB = 95;
    i.diskMB = 280;
    if (i.state == State::Error) i.error = "hash";
    return i;
}
bool Install(const char* modId, std::string* reason) {
    if (fake_media::g_packState == State::Downloading) {
        *reason = "busy";
        return false;
    }
    ++fake_media::g_installs;
    fake_media::g_installMod = modId;
    fake_media::g_packState = State::Downloading;
    return true;
}
void CancelInstall() { fake_media::g_packState = State::Missing; }
}  // namespace media::pack

namespace media::host {
namespace {
fake_media::Player* FakeOwn(const std::string& mod, uint32_t id) {
    auto it = fake_media::g_players.find(id);
    return it == fake_media::g_players.end() || it->second.mod != mod ? nullptr : &it->second;
}
bool PackOk() { return fake_media::g_packState == pack::State::Ready; }
}  // namespace
bool Available(std::string* reason) {
    if (!fake_media::g_available) *reason = fake_media::g_reason;
    return fake_media::g_available;
}
void Touch(const std::string& mod) { fake_media::g_touched.insert(mod); }
uint32_t Open(const std::string& mod, const OpenSpec& spec, std::string* reason) {
    if (!Available(reason)) return 0;
    for (const auto& [id, p] : fake_media::g_players)
        if (p.mod == mod) {
            *reason = "busy";
            return 0;
        }
    if (!PackOk()) {
        *reason = "pack";
        return 0;
    }
    if (spec.torrent) {
        auto it = fake_media::g_torrents.find(spec.torrent);
        if (it == fake_media::g_torrents.end() || it->second.mod != mod) {
            *reason = "closed";
            return 0;
        }
        bool ok = false;
        for (const File& f : it->second.files) ok |= f.index == spec.file && f.video;
        if (!ok) {
            *reason = "unsupported-url";
            return 0;
        }
    }
    fake_media::Player p;
    p.mod = mod;
    p.spec = spec;
    p.paused = spec.paused;
    p.speed = spec.speed;
    p.volume = spec.volume;
    p.quality = spec.quality;
    const uint32_t id = fake_media::g_next++;
    fake_media::g_players[id] = p;
    return id;
}
bool SetPaused(const std::string& mod, uint32_t id, bool paused) {
    auto* p = FakeOwn(mod, id);
    if (p) p->paused = paused;
    return p != nullptr;
}
bool Seek(const std::string& mod, uint32_t id, double t, const std::string& mode) {
    auto* p = FakeOwn(mod, id);
    if (p) {
        p->lastSeek = t;
        p->lastSeekMode = mode;
    }
    return p != nullptr;
}
bool SetSpeed(const std::string& mod, uint32_t id, double speed) {
    auto* p = FakeOwn(mod, id);
    if (p) p->speed = speed;
    return p != nullptr;
}
bool SetVolume(const std::string& mod, uint32_t id, double volume) {
    auto* p = FakeOwn(mod, id);
    if (p) p->volume = volume;
    return p != nullptr;
}
bool SetMute(const std::string& mod, uint32_t id, bool mute) {
    auto* p = FakeOwn(mod, id);
    if (p) p->mute = mute;
    return p != nullptr;
}
bool SetTrack(const std::string& mod, uint32_t id, bool audio, int track) {
    auto* p = FakeOwn(mod, id);
    if (p) (audio ? p->aid : p->sid) = track;
    return p != nullptr;
}
bool SetQuality(const std::string& mod, uint32_t id, int height) {
    auto* p = FakeOwn(mod, id);
    if (p) p->quality = height;
    return p != nullptr;
}
bool SetSize(const std::string& mod, uint32_t id, int w, int h) {
    auto* p = FakeOwn(mod, id);
    if (p) {
        p->w = w;
        p->h = h;
    }
    return p != nullptr;
}
bool Follow(const std::string& mod, uint32_t id, const FollowState* st, uint64_t hostId) {
    auto* p = FakeOwn(mod, id);
    if (!p) return false;
    p->following = st != nullptr;
    if (st) p->follow = *st;
    p->hostId = hostId;
    return true;
}
bool State(const std::string& mod, uint32_t id, PlayerState* out) {
    auto* p = FakeOwn(mod, id);
    if (!p) return false;
    out->time = 12.5;
    out->duration = 120;
    out->paused = p->paused;
    out->speed = p->speed;
    out->title = "Fake";
    out->w = 1920;
    out->h = 1080;
    out->aspect = 16.0 / 9.0;
    out->audio = {{1, "en", "English", true}};
    out->sub = {{2, "en", "English", false}, {3, "de", "Deutsch", true}};
    out->following = p->following;
    out->hasDrift = p->following;
    out->drift = p->following ? 0.05 : 0;
    out->syncAction = p->following ? "nudge" : "none";
    out->helper = "ready";
    return true;
}
bool Close(const std::string& mod, uint32_t id) {
    if (!FakeOwn(mod, id)) return false;
    fake_media::g_players.erase(id);
    return true;
}
uint32_t Resolve(const std::string& mod, const std::string& canonicalUrl, uint32_t token, std::string* reason) {
    if (!Available(reason)) return 0;
    int n = 0;
    for (const auto& [rid, r] : fake_media::g_resolves) n += r.mod == mod;
    if (n >= 4) {
        *reason = "busy";
        return 0;
    }
    if (!PackOk()) {
        *reason = "pack";
        return 0;
    }
    const uint32_t rid = fake_media::g_next++;
    fake_media::g_resolves[rid] = {mod, canonicalUrl, token};
    return rid;
}
void CancelResolve(const std::string& mod, uint32_t rid) {
    auto it = fake_media::g_resolves.find(rid);
    if (it != fake_media::g_resolves.end() && it->second.mod == mod) fake_media::g_resolves.erase(it);
}
uint32_t AddTorrent(const std::string& mod, const std::string& magnet, uint32_t token, bool inMatch, std::string* reason) {
    if (!Available(reason)) return 0;
    fake_media::g_inMatchSeen = inMatch;
    if (inMatch) {
        *reason = "first-use-in-match";
        return 0;
    }
    int n = 0;
    for (const auto& [id, t] : fake_media::g_torrents) n += t.mod == mod;
    if (n >= 4) {
        *reason = "busy";
        return 0;
    }
    if (!PackOk()) {
        *reason = "pack";
        return 0;
    }
    const uint32_t id = fake_media::g_next++;
    fake_media::g_torrents[id] = {mod, magnet, token};
    return id;
}
bool TorrentFiles(const std::string& mod, uint32_t id, std::vector<File>* out, bool* have) {
    auto it = fake_media::g_torrents.find(id);
    if (it == fake_media::g_torrents.end() || it->second.mod != mod) return false;
    *have = it->second.haveFiles;
    *out = it->second.files;
    return true;
}
bool TorrentStatusOf(const std::string& mod, uint32_t id, TorrentStatus* out) {
    auto it = fake_media::g_torrents.find(id);
    if (it == fake_media::g_torrents.end() || it->second.mod != mod) return false;
    out->peers = 3;
    out->down = 1000;
    out->metadata = it->second.haveFiles;
    return true;
}
bool CloseTorrent(const std::string& mod, uint32_t id) {
    auto it = fake_media::g_torrents.find(id);
    if (it == fake_media::g_torrents.end() || it->second.mod != mod) return false;
    fake_media::g_torrents.erase(it);
    return true;
}
void SetSink(Sink* sink) { fake_media::g_sink = sink; }
}  // namespace media::host
