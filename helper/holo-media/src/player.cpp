#include "player.h"

#include <cmath>
#include <cstring>

#include "bt_bridge.h"
#include "frame_size.h"
#include "log.h"
#include "mpv_api.h"
#include "mpv_opts.h"
#include "util.h"

namespace holo {
namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;
namespace shm = melange::media::shm;
namespace url = melange::media::url;

std::atomic<int> Player::teardowns_{0};

bool Player::WaitTeardowns(uint32_t ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (teardowns_.load() > 0 && GetTickCount64() < until) Sleep(10);
    return teardowns_.load() == 0;
}

#if !HOLO_HAVE_MPV
// A build without the libmpv headers: no player at all (every open answers error{code="load"}).
std::unique_ptr<Player> Player::Create(uint32_t, const PlayerEnv&, std::string* why) {
    if (why) *why = "this holo-media build has no player (built without HOLO_MPV_DIR)";
    return nullptr;
}
Player::~Player() = default;
bool Player::Load(const LoadSpec&, std::string*, std::string*) { return false; }
void Player::Set(const pipe::SetCmd&) {}
void Player::Seek(const pipe::SeekCmd&) {}
void Player::Size(int, int) {}
void Player::Stop() {}
void Player::Drain() {}
void Player::Tick() {}
void Player::Close(std::unique_ptr<Player>) {}
std::string Player::GetString(const char*) { return {}; }
#else
namespace {
using mpv::A;

// The observed properties; reply_userdata = index + 1. thr: index into Player::thr_ (-1 = sent on every change).
struct PropDef {
    const char* name;
    mpv_format fmt;
    int thr;
    double minGapMs;
};
const PropDef kProps[] = {
    {proto::prop::kTimePos, MPV_FORMAT_DOUBLE, 0, 1000.0 / proto::kTimePosHz},
    {proto::prop::kAudioPts, MPV_FORMAT_DOUBLE, 1, 1000.0 / proto::kTimePosHz},
    {proto::prop::kPause, MPV_FORMAT_FLAG, -1, 0},
    {proto::prop::kPausedForCache, MPV_FORMAT_FLAG, -1, 0},
    {proto::prop::kDuration, MPV_FORMAT_DOUBLE, -1, 0},
    {proto::prop::kTrackList, MPV_FORMAT_NODE, -1, 0},
    {proto::prop::kCacheTime, MPV_FORMAT_DOUBLE, 2, 250},
    {proto::prop::kEofReached, MPV_FORMAT_FLAG, -1, 0},
    {proto::prop::kVideoParams, MPV_FORMAT_NODE, -1, 0},
    {proto::prop::kMediaTitle, MPV_FORMAT_STRING, -1, 0},
    {proto::prop::kSpeed, MPV_FORMAT_DOUBLE, -1, 0},
};

const mpv_node* MapGet(const mpv_node& n, const char* key) {
    if (n.format != MPV_FORMAT_NODE_MAP || !n.u.list) return nullptr;
    for (int i = 0; i < n.u.list->num; ++i)
        if (strcmp(n.u.list->keys[i], key) == 0) return &n.u.list->values[i];
    return nullptr;
}
int64_t NodeInt(const mpv_node* n) {
    if (!n) return 0;
    if (n->format == MPV_FORMAT_INT64) return n->u.int64;
    if (n->format == MPV_FORMAT_DOUBLE && std::isfinite(n->u.double_)) return static_cast<int64_t>(n->u.double_);
    return 0;
}
double NodeDouble(const mpv_node* n) {
    if (!n) return 0;
    if (n->format == MPV_FORMAT_DOUBLE) return n->u.double_;
    if (n->format == MPV_FORMAT_INT64) return static_cast<double>(n->u.int64);
    return 0;
}
std::string NodeStr(const mpv_node* n) { return n && n->format == MPV_FORMAT_STRING && n->u.string ? n->u.string : ""; }
bool NodeFlag(const mpv_node* n) { return n && n->format == MPV_FORMAT_FLAG && n->u.flag; }

// A key-value list option as an mpv node map: each pair stays whole, whatever its characters.
struct NodeMap {
    std::vector<std::string> keys, vals;
    std::vector<char*> kp;
    std::vector<mpv_node> vn;
    mpv_node_list list{};
    mpv_node node{};
    explicit NodeMap(const opts::Pairs& pairs) {
        for (const auto& [k, v] : pairs) {
            keys.push_back(k);
            vals.push_back(v);
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            kp.push_back(keys[i].data());
            mpv_node n{};
            n.format = MPV_FORMAT_STRING;
            n.u.string = vals[i].data();
            vn.push_back(n);
        }
        list.num = static_cast<int>(keys.size());
        list.keys = kp.data();
        list.values = vn.data();
        node.format = MPV_FORMAT_NODE_MAP;
        node.u.list = &list;
    }
};

double Clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

void OnWakeup(void* ev) { SetEvent(static_cast<HANDLE>(ev)); }
void OnRenderUpdate(void* ev) { SetEvent(static_cast<HANDLE>(ev)); }
}  // namespace

std::unique_ptr<Player> Player::Create(uint32_t id, const PlayerEnv& env, std::string* why) {
    if (!A().mpv_create) {
        if (why) *why = "libmpv is not loaded";
        return nullptr;
    }
    std::unique_ptr<Player> p(new Player(id, env));
    if (!p->Init(why)) {
        Close(std::move(p));
        return nullptr;
    }
    return p;
}

bool Player::Init(std::string* why) {
    h_ = A().mpv_create();
    if (!h_) {
        if (why) *why = "mpv_create failed";
        return false;
    }
    for (const auto& o : opts::BaseOptions(env_.mpvHome, env_.hello.hwdec, env_.hello.audioDelayMs, env_.caFile)) {
        const int rc = A().mpv_set_option_string(h_, o.name, o.value.c_str());
        if (rc >= 0) continue;
        if (o.optional && rc == MPV_ERROR_OPTION_NOT_FOUND) {
            HLOG("player %u: mpv has no option %s (that script does not exist in this libmpv)", id_, o.name);
            continue;
        }
        if (why) *why = std::string("mpv option ") + o.name + ": " + A().mpv_error_string(rc);
        return false;
    }
    {
        NodeMap so(opts::ScriptOpts(env_.ytdlp));
        NodeMap raw(opts::YtdlRawOptions(env_.deno, ""));
        int rc = A().mpv_set_option(h_, "script-opts", MPV_FORMAT_NODE, &so.node);
        if (rc >= 0) rc = A().mpv_set_option(h_, "ytdl-raw-options", MPV_FORMAT_NODE, &raw.node);
        if (rc < 0) {
            if (why) *why = std::string("mpv ytdl options: ") + A().mpv_error_string(rc);
            return false;
        }
    }
    A().mpv_request_log_messages(h_, "warn");
    int rc = A().mpv_initialize(h_);
    if (rc < 0) {
        if (why) *why = std::string("mpv_initialize: ") + A().mpv_error_string(rc);
        return false;
    }
    if (env_.bt) {
        rc = A().mpv_stream_cb_add_ro(h_, bt::kStreamProtocol, env_.bt, [](void* ud, char* uri, mpv_stream_cb_info* info) {
            return BtBridge::Open(ud, uri, info);
        });
        if (rc < 0) HLOG("player %u: holobt protocol not registered: %s", id_, A().mpv_error_string(rc));
    }
    char apiSw[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, apiSw}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    rc = A().mpv_render_context_create(&rc_, h_, params);
    if (rc < 0) {
        rc_ = nullptr;
        if (why) *why = std::string("mpv software renderer: ") + A().mpv_error_string(rc);
        return false;
    }
    renderWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    A().mpv_render_context_set_update_callback(rc_, OnRenderUpdate, renderWake_);
    for (size_t i = 0; i < std::size(kProps); ++i) A().mpv_observe_property(h_, i + 1, kProps[i].name, kProps[i].fmt);
    A().mpv_set_wakeup_callback(h_, OnWakeup, env_.wake);
    render_ = std::thread([this] { RenderLoop(); });
    return true;
}

Player::~Player() {
    // Only ever reached on the teardown thread (Close), or for a player whose Init never started a thread.
    UnmapSection();
    if (renderWake_) CloseHandle(renderWake_);
}

void Player::Teardown() {
    stop_ = true;
    if (renderWake_) SetEvent(renderWake_);
    if (render_.joinable()) render_.join();
    if (rc_) A().mpv_render_context_free(rc_);  // before the core goes away (render.h)
    rc_ = nullptr;
    if (h_) {
        A().mpv_set_wakeup_callback(h_, nullptr, nullptr);
        A().mpv_terminate_destroy(h_);
    }
    h_ = nullptr;
}

void Player::Close(std::unique_ptr<Player> p) {
    if (!p) return;
    ++teardowns_;
    std::thread([q = std::move(p)]() mutable {
        q->Teardown();
        q.reset();
        --teardowns_;
    }).detach();
}

bool Player::MapSection(const std::string& name, std::string* why) {
    if (name == secName_ && secView_) return true;
    HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, Wide(name).c_str());
    if (!m) {
        if (why) *why = "OpenFileMapping: " + WinError(GetLastError());
        return false;
    }
    void* v = MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi{};
    // The core creates the section with its own size: refuse one too small for three full slots.
    if (!v || !VirtualQuery(v, &mbi, sizeof mbi) || mbi.RegionSize < shm::SectionBytes()) {
        if (why) *why = v ? "the frame section is too small" : "MapViewOfFile: " + WinError(GetLastError());
        if (v) UnmapViewOfFile(v);
        CloseHandle(m);
        return false;
    }
    auto w = std::make_unique<shm::Writer>(v);
    if (!w->HeaderOk()) {
        if (why) *why = "the frame section has no HMF1 header";
        UnmapViewOfFile(v);
        CloseHandle(m);
        return false;
    }
    shm::Word(static_cast<shm::Header*>(v), &shm::Header::writerPid).store(GetCurrentProcessId());
    std::lock_guard lk(secMu_);
    if (secView_) UnmapViewOfFile(secView_);
    if (secMap_) CloseHandle(secMap_);
    secMap_ = m;
    secView_ = v;
    secName_ = name;
    writer_ = std::move(w);
    return true;
}

void Player::UnmapSection() {
    std::lock_guard lk(secMu_);
    writer_.reset();
    if (secView_) UnmapViewOfFile(secView_);
    if (secMap_) CloseHandle(secMap_);
    secView_ = nullptr;
    secMap_ = nullptr;
    secName_.clear();
}

void Player::RenderLoop() {
    char fmt[] = "bgr0";
    uint32_t frameNo = 0;
    bool loggedErr = false;
    while (!stop_.load()) {
        WaitForSingleObject(renderWake_, 100);
        if (stop_.load()) break;
        const uint64_t flags = A().mpv_render_context_update(rc_);
        const bool force = force_.exchange(false) && vidW_.load() > 0;
        if (!(flags & MPV_RENDER_UPDATE_FRAME) && !force) continue;
        std::lock_guard lk(secMu_);
        if (!writer_) continue;
        const Dims d = FitFrame(boxW_.load(), boxH_.load(), vidW_.load(), vidH_.load(), aspect_.load());
        const uint32_t slot = writer_->Begin();
        int size[2] = {d.w, d.h};
        size_t stride = static_cast<size_t>(d.w) * 4;
        mpv_render_param p[] = {{MPV_RENDER_PARAM_SW_SIZE, size},
                                {MPV_RENDER_PARAM_SW_FORMAT, fmt},
                                {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                                {MPV_RENDER_PARAM_SW_POINTER, writer_->Data(slot)},
                                {MPV_RENDER_PARAM_INVALID, nullptr}};
        const int rc = A().mpv_render_context_render(rc_, p);
        if (rc < 0) {
            if (!loggedErr) HLOG("player %u: render: %s", id_, A().mpv_error_string(rc));
            loggedErr = true;
            continue;
        }
        shm::SlotMeta m{};
        m.w = static_cast<uint32_t>(d.w);
        m.h = static_cast<uint32_t>(d.h);
        m.stride = static_cast<uint32_t>(stride);
        m.fmt = shm::kFmtBgr0;
        m.frameNo = ++frameNo;
        m.pts = -1;
        writer_->Publish(slot, m);
        ++frames_;
    }
}

bool Player::Load(const LoadSpec& s, std::string* code, std::string* why) {
    if (!MapSection(s.shm, why)) {
        if (code) *code = proto::err::kShm;
        return false;
    }
    ih_ = s.ih;
    pendingGens_.push_back(s.gen);
    // Every per-item setting is (re)set before the loadfile, in order on mpv's queue, so nothing leaks between items.
    auto setStr = [&](const char* name, const std::string& v) {
        const char* c = v.c_str();
        A().mpv_set_property_async(h_, 0, name, MPV_FORMAT_STRING, &c);
    };
    auto setFlag = [&](const char* name, bool b) {
        int f = b ? 1 : 0;
        A().mpv_set_property_async(h_, 0, name, MPV_FORMAT_FLAG, &f);
    };
    auto setNum = [&](const char* name, double v) { A().mpv_set_property_async(h_, 0, name, MPV_FORMAT_DOUBLE, &v); };
    const std::string alang = url::ValidLang(s.alang) ? s.alang : "", slang = url::ValidLang(s.slang) ? s.slang : "";
    setFlag("pause", s.paused);
    setFlag("mute", s.mute);
    setNum("volume", Clamp(s.volume, 0, 100));
    setNum("speed", Clamp(s.speed, 0.25, 4));
    char start[32];
    snprintf(start, sizeof start, "%.3f", Clamp(s.start, 0, 1e7));
    setStr("start", start);
    setStr("alang", alang);
    setStr("slang", slang);
    setStr("ytdl", s.ytdl ? "yes" : "no");
    // References stay off for everything but YouTube items: mpv's ytdl_hook joins YouTube's separate video and audio
    // streams (and its subtitles) through an edl:// it builds itself from yt-dlp's answer, which needs them. Torrent
    // data never gets them (playlists, EDL and mov references inside a hostile file, Critique S3).
    setStr("access-references", opts::AccessReferences(s.ytdl));
    setStr("ytdl-format", opts::YtdlFormat(opts::CapHeight(s.height, env_.hello.maxHeight)));
    {
        NodeMap raw(opts::YtdlRawOptions(env_.deno, slang));
        A().mpv_set_property_async(h_, 0, "ytdl-raw-options", MPV_FORMAT_NODE, &raw.node);
    }
    const char* cmd[] = {"loadfile", s.url.c_str(), "replace", nullptr};
    const int rc = A().mpv_command_async(h_, 0, cmd);
    if (rc < 0) {
        pendingGens_.pop_back();
        if (code) *code = proto::err::kLoad;
        if (why) *why = std::string("loadfile: ") + A().mpv_error_string(rc);
        return false;
    }
    return true;
}

void Player::Set(const pipe::SetCmd& c) {
    namespace sp = proto::setprop;
    if (c.prop == sp::kPause || c.prop == sp::kMute) {
        int f = c.boolean ? 1 : 0;
        A().mpv_set_property_async(h_, 0, c.prop.c_str(), MPV_FORMAT_FLAG, &f);
    } else if (c.prop == sp::kVolume || c.prop == sp::kSpeed) {
        double v = c.prop == sp::kVolume ? Clamp(c.number, 0, 100) : Clamp(c.number, 0.25, 4);
        A().mpv_set_property_async(h_, 0, c.prop.c_str(), MPV_FORMAT_DOUBLE, &v);
    } else if (c.prop == sp::kAid || c.prop == sp::kSid) {
        if (c.isBool) {
            const char* no = "no";
            A().mpv_set_property_async(h_, 0, c.prop.c_str(), MPV_FORMAT_STRING, &no);
        } else {
            int64_t t = static_cast<int64_t>(c.number);
            A().mpv_set_property_async(h_, 0, c.prop.c_str(), MPV_FORMAT_INT64, &t);
        }
    }
}

void Player::Seek(const pipe::SeekCmd& c) {
    char pos[32];
    snprintf(pos, sizeof pos, "%.3f", Clamp(c.t, -1e7, 1e7));
    const char* flags = c.mode == "keyframes" ? "absolute+keyframes" : c.mode == "relative" ? "relative+exact" : "absolute+exact";
    const char* cmd[] = {"seek", pos, flags, nullptr};
    A().mpv_command_async(h_, 0, cmd);
}

void Player::Size(int w, int h) {
    boxW_ = w;
    boxH_ = h;
    force_ = true;  // re-render the current picture at the new size, even while paused
    SetEvent(renderWake_);
}

void Player::Stop() {
    const char* cmd[] = {"stop", nullptr};
    A().mpv_command_async(h_, 0, cmd);
    ih_.clear();
}

std::string Player::GetString(const char* prop) {
    char* s = A().mpv_get_property_string(h_, prop);
    std::string r = s ? s : "";
    if (s) A().mpv_free(s);
    return r;
}

void Player::Drain() {
    for (int n = 0; n < 1000; ++n) {
        mpv_event* ev = A().mpv_wait_event(h_, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        OnEvent(ev);
    }
}

void Player::Emit(pipe::PropEvt& e, int slot) {
    e.id = id_;
    e.gen = activeGen_;
    if (slot < 0) {
        env_.send(pipe::Message{e}, false);
        return;
    }
    Throttled& t = thr_[slot];
    t.last = e;
    const double now = QpcMs();
    if (now - t.sentMs >= kProps[slot == 0 ? 0 : slot == 1 ? 1 : 6].minGapMs) {
        env_.send(pipe::Message{t.last}, true);
        t.sentMs = now;
        t.pending = false;
    } else {
        t.pending = true;
    }
}

void Player::Tick() {
    const double now = QpcMs();
    for (int s = 0; s < 3; ++s) {
        Throttled& t = thr_[s];
        if (t.pending && now - t.sentMs >= kProps[s == 0 ? 0 : s == 1 ? 1 : 6].minGapMs) {
            env_.send(pipe::Message{t.last}, true);
            t.sentMs = now;
            t.pending = false;
        }
    }
}

void Player::OnProperty(void* p) {
    auto* ev = static_cast<mpv_event*>(p);
    auto* pr = static_cast<mpv_event_property*>(ev->data);
    if (ev->reply_userdata < 1 || ev->reply_userdata > std::size(kProps)) return;
    const PropDef& def = kProps[ev->reply_userdata - 1];
    using K = pipe::PropEvt::Kind;
    pipe::PropEvt e;
    e.name = def.name;
    if (pr->format == MPV_FORMAT_NONE || !pr->data) {
        e.kind = K::None;
    } else if (def.fmt == MPV_FORMAT_DOUBLE) {
        const double v = *static_cast<double*>(pr->data);
        if (!std::isfinite(v)) {
            e.kind = K::None;
        } else {
            const bool pos = def.thr == 0 || def.thr == 1;
            e.kind = K::Number;
            e.number = Clamp(v, pos ? -1e3 : 0, 1e7);
            if (pos) e.qpcMs = QpcMs();
        }
    } else if (def.fmt == MPV_FORMAT_FLAG) {
        e.kind = K::Bool;
        e.boolean = *static_cast<int*>(pr->data) != 0;
        if (e.name == proto::prop::kEofReached) {
            // keep-open=yes holds the last frame at the end instead of ending the file, so mpv sends no end-file
            // there: the first eof-reached of a load stands in for end-file{reason="eof"}.
            if (e.boolean && eofGen_ != activeGen_ + 1u) {
                eofGen_ = activeGen_ + 1u;
                pipe::EventEvt end;
                end.id = id_;
                end.gen = activeGen_;
                end.name = proto::event::kEndFile;
                end.reason = "eof";
                Emit(e, def.thr);
                env_.send(pipe::Message{end}, false);
                return;
            }
            if (!e.boolean && eofGen_ == activeGen_ + 1u) eofGen_ = 0;  // seeked back from the end: the next end counts again
        }
    } else if (def.fmt == MPV_FORMAT_STRING) {
        e.kind = K::String;
        e.str = url::CleanName(*static_cast<char**>(pr->data), proto::kMaxTitleBytes);
    } else if (e.name == proto::prop::kTrackList) {
        e.kind = K::Tracks;
        const mpv_node& n = *static_cast<mpv_node*>(pr->data);
        size_t counts[3] = {};
        if (n.format == MPV_FORMAT_NODE_ARRAY && n.u.list) {
            for (int i = 0; i < n.u.list->num; ++i) {
                const mpv_node& t = n.u.list->values[i];
                pipe::Track tr;
                tr.type = NodeStr(MapGet(t, "type"));
                const int k = tr.type == "audio" ? 0 : tr.type == "sub" ? 1 : tr.type == "video" ? 2 : -1;
                const int64_t id = NodeInt(MapGet(t, "id"));
                if (k < 0 || id < 1 || id > 9999 || ++counts[k] > proto::kMaxTracks) continue;
                tr.id = static_cast<int>(id);
                tr.lang = NodeStr(MapGet(t, "lang"));
                if (!url::ValidLang(tr.lang)) tr.lang.clear();
                tr.title = url::CleanName(NodeStr(MapGet(t, "title")), proto::kMaxTitleBytes);
                tr.selected = NodeFlag(MapGet(t, "selected"));
                e.tracks.push_back(std::move(tr));
            }
        }
        // Many tracks with long titles: shorten the titles until the line fits.
        for (size_t cap = 128; pipe::Encode(pipe::Message{e}).size() > proto::kMaxLine && cap >= 8; cap /= 4)
            for (auto& tr : e.tracks) tr.title = url::CleanName(tr.title, cap);
    } else if (e.name == proto::prop::kVideoParams) {
        const mpv_node& n = *static_cast<mpv_node*>(pr->data);
        int64_t w = NodeInt(MapGet(n, "dw")), h = NodeInt(MapGet(n, "dh"));
        if (w <= 0 || h <= 0) {
            w = NodeInt(MapGet(n, "w"));
            h = NodeInt(MapGet(n, "h"));
        }
        double aspect = NodeDouble(MapGet(n, "aspect"));
        if (!(aspect > 0 && aspect < 100)) aspect = h > 0 ? static_cast<double>(w) / h : 0;
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !(aspect >= 0 && aspect <= 100)) {
            e.kind = K::None;
        } else {
            e.kind = K::Video;
            e.video = {static_cast<int>(w), static_cast<int>(h), aspect};
            vidW_ = static_cast<int>(w);
            vidH_ = static_cast<int>(h);
            aspect_ = aspect;
        }
    } else {
        return;
    }
    Emit(e, def.thr);
}

void Player::OnEvent(void* p) {
    auto* ev = static_cast<mpv_event*>(p);
    switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: OnProperty(ev); break;
        case MPV_EVENT_START_FILE: {
            const auto* sf = static_cast<mpv_event_start_file*>(ev->data);
            if (!pendingGens_.empty()) {
                activeGen_ = pendingGens_.front();
                pendingGens_.pop_front();
            }
            if (sf) entryGen_[sf->playlist_entry_id] = activeGen_;
            for (auto& t : thr_) t.pending = false;
            eofGen_ = 0;
            break;
        }
        case MPV_EVENT_FILE_LOADED: {
            pipe::EventEvt e;
            e.id = id_;
            e.gen = activeGen_;
            e.name = proto::event::kFileLoaded;
            env_.send(pipe::Message{e}, false);
            HLOG("player %u gen %u: loaded", id_, activeGen_);
            force_ = true;
            SetEvent(renderWake_);
            break;
        }
        case MPV_EVENT_END_FILE: {
            const auto* ef = static_cast<mpv_event_end_file*>(ev->data);
            uint32_t gen = activeGen_;
            if (auto it = entryGen_.find(ef->playlist_entry_id); it != entryGen_.end()) {
                gen = it->second;
                entryGen_.erase(it);
            }
            pipe::EventEvt e;
            e.id = id_;
            e.gen = gen;
            e.name = proto::event::kEndFile;
            e.reason = ef->reason == MPV_END_FILE_REASON_EOF ? "eof" : ef->reason == MPV_END_FILE_REASON_ERROR ? "error" : "stop";
            if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                pipe::ErrorEvt er;
                er.id = id_;
                er.gen = gen;
                er.code = proto::err::kLoad;
                er.message = A().mpv_error_string(ef->error);
                env_.send(pipe::Message{er}, false);
                HLOG("player %u gen %u: load failed: %s", id_, gen, er.message.c_str());
            }
            if (!(e.reason == "eof" && eofGen_ == gen + 1u)) env_.send(pipe::Message{e}, false);  // not twice
            break;
        }
        case MPV_EVENT_PLAYBACK_RESTART:
            if (playingGen_ != activeGen_ + 1u) {  // once per load: what mpv decodes with and plays through
                playingGen_ = activeGen_ + 1u;
                HLOG("player %u gen %u: playing (ao %s, hwdec %s, vo %s)", id_, activeGen_, GetString("current-ao").c_str(),
                     GetString("hwdec-current").c_str(), GetString("current-vo").c_str());
            }
            break;
        case MPV_EVENT_LOG_MESSAGE: {
            const auto* lm = static_cast<mpv_event_log_message*>(ev->data);
            if (++logLines_ <= 400) {
                std::string text = lm->text ? lm->text : "";
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                HLOG("mpv %u [%s] %s: %s", id_, lm->prefix ? lm->prefix : "", lm->level ? lm->level : "", text.c_str());
            }
            break;
        }
        case MPV_EVENT_SET_PROPERTY_REPLY:
        case MPV_EVENT_COMMAND_REPLY:
            if (ev->error < 0) HLOG("player %u: %s failed: %s", id_, A().mpv_event_name(ev->event_id), A().mpv_error_string(ev->error));
            break;
        default: break;
    }
}
#endif
}  // namespace holo
