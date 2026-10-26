#include "app.h"

#include <thread>
#include <vector>

#include "log.h"
#include "mpv_api.h"
#include "sandbox.h"
#include "util.h"

namespace holo {
namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;
namespace url = melange::media::url;

namespace {
constexpr size_t kMaxPlayers = 8;  // helper-wide (the core allows one per mod)
constexpr uint32_t kConnectMs = 10000;
}  // namespace

App::App(const Args& a) : args_(a) { wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

void App::ProbeVersions(const std::string& mpvHome, std::string* mpvVer, std::string* ffmpegVer) {
#if HOLO_HAVE_MPV
    const auto& A = mpv::A();
    if (!A.mpv_create) return;
    mpv_handle* h = A.mpv_create();
    if (!h) return;
    const char* const kOpts[][2] = {{"config", "no"}, {"config-dir", mpvHome.c_str()}, {"load-scripts", "no"}, {"osc", "no"},
                                    {"ytdl", "no"},   {"terminal", "no"},              {"vo", "null"},         {"ao", "null"},
                                    {"idle", "yes"}};
    for (const auto& o : kOpts) A.mpv_set_option_string(h, o[0], o[1]);
    if (A.mpv_initialize(h) >= 0) {
        for (auto [prop, dst] : {std::pair{"mpv-version", mpvVer}, std::pair{"ffmpeg-version", ffmpegVer}}) {
            char* s = A.mpv_get_property_string(h, prop);
            if (s) {
                *dst = url::CleanName(s, proto::kMaxShortBytes);
                A.mpv_free(s);
            }
        }
    }
    A.mpv_terminate_destroy(h);
#else
    (void)mpvHome;
    (void)mpvVer;
    (void)ffmpegVer;
#endif
}

void App::Error(uint32_t id, uint32_t gen, const std::string& ih, const char* code, const std::string& message) {
    pipe::ErrorEvt e;
    e.id = id;
    e.gen = gen;
    e.ih = ih;
    e.code = code;
    e.message = url::CleanName(message, proto::kMaxMessageBytes);
    pipe_.Send(pipe::Message{e});
}

int App::Run() {
    std::string why;
    if (!sandbox::PrepareCache(args_.cache, &why)) {
        fprintf(stderr, "holo-media: %s\n", why.c_str());
        return 3;
    }
    log::Open(args_.cache + L"\\holo-media.log");
    HLOG("holo-media %s (protocol %d), pid %lu, parent %u, integrity %s", HOLO_MEDIA_VERSION, proto::kProto, GetCurrentProcessId(),
         args_.parent, sandbox::IntegrityName(sandbox::IntegrityRid()));
    sandbox::ScrubEnvironment(args_.cache);
    sandbox::ApplyMitigations();
    sandbox::JoinOwnJob();
    HANDLE lock = sandbox::HoldLock(args_.cache);
    (void)lock;  // held until the process ends
    if (args_.sweep) sandbox::SweepDeadInstances(args_.cache);

    parent_ = OpenProcess(SYNCHRONIZE, FALSE, args_.parent);
    if (!parent_) HLOG("parent: not watchable (%s); the pipe closing ends the helper", WinError(GetLastError()).c_str());

#if HOLO_HAVE_MPV
    if (!mpv::Load(args_.libmpv, &why)) libmpvError_ = why;
#else
    libmpvError_ = "this holo-media build has no player";
#endif
    if (!libmpvError_.empty()) HLOG("libmpv: %s", libmpvError_.c_str());
    int roots = 0;
    const std::wstring ca = args_.cache + L"\\home\\roots.pem";
    if (sandbox::WriteRootBundle(ca, &roots)) env_.caFile = Utf8(ca);
    HLOG("tls: %d trusted roots exported for mpv", roots);
    env_.mpvHome = Utf8(args_.cache + L"\\mpvhome");
    env_.ytdlp = Utf8(args_.ytdlp);
    env_.deno = Utf8(args_.deno);
    env_.wake = wake_;
    env_.send = [this](const pipe::Message& m, bool droppable) { pipe_.Send(m, droppable); };
    bridge_ = std::make_unique<BtBridge>(wake_);
    env_.bt = bridge_.get();

    pipe::HelloEvt hello;
    if (libmpvError_.empty()) ProbeVersions(env_.mpvHome, &hello.mpv, &hello.ffmpeg);
    hello.hwdec = env_.hello.hwdec;
    hello.libtorrent = bt::EngineVersion();
    if (!pipe_.Connect(args_.pipe, args_.parent, kConnectMs, &why)) {
        HLOG("pipe: %s", why.c_str());
        return 3;
    }
    pipe_.Start();
    pipe_.Send(pipe::Message{hello});
    HLOG("connected; mpv %s, %s", hello.mpv.empty() ? "-" : hello.mpv.c_str(), hello.ffmpeg.empty() ? "-" : hello.ffmpeg.c_str());
    resolver_ = std::make_unique<Resolver>(args_.ytdlp, args_.deno, args_.cache + L"\\tmp",
                                           [this](const pipe::Message& m) { pipe_.Send(m); });

    int code = 0;
    while (!quit_) {
        const HANDLE w[3] = {pipe_.InboxEvent(), wake_, parent_};
        const DWORD r = WaitForMultipleObjects(parent_ ? 3 : 2, w, FALSE, 50);
        if (parent_ && r == WAIT_OBJECT_0 + 2) {
            HLOG("the game exited");
            code = 1;
            break;
        }
        pipe::Message m;
        while (!quit_ && pipe_.Pop(&m)) Handle(m);
        if (pipe_.Broken()) {
            HLOG("the pipe closed");
            code = 1;
            break;
        }
        bridge_->Service();
        for (auto& [id, p] : players_) {
            p->Drain();
            p->Tick();
        }
        PollEngine();
    }
    Shutdown();
    return code;
}

void App::Shutdown() {
    // Nothing below may hang the exit: a watchdog ends the process after 10 s whatever happens.
    std::thread([] {
        Sleep(10000);
        HLOG("shutdown took too long, terminating");
        TerminateProcess(GetCurrentProcess(), 4);
    }).detach();
    bridge_->Shutdown();
    for (auto& [id, p] : players_) Player::Close(std::move(p));
    players_.clear();
    if (engine_) {
        bt::Engine* e = engine_.get();
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread([e, done] {
            e->Shutdown();
            SetEvent(done);
        }).detach();
        if (WaitForSingleObject(done, 6000) != WAIT_OBJECT_0) HLOG("torrent engine did not shut down in time");
    }
    if (!Player::WaitTeardowns(3000)) HLOG("a player did not tear down in time");
    pipe_.Flush(500);
    HLOG("exit");
}

void App::Handle(pipe::Message& m) {
    if (auto* c = std::get_if<pipe::HelloCmd>(&m)) {
        if (c->proto != proto::kProto) HLOG("core speaks protocol %d, this helper %d", c->proto, proto::kProto);
        env_.hello = *c;
    } else if (auto* o = std::get_if<pipe::OpenCmd>(&m)) {
        OnOpen(*o);
    } else if (auto* s = std::get_if<pipe::SetCmd>(&m)) {
        if (auto it = players_.find(s->id); it != players_.end()) it->second->Set(*s);
        else Error(s->id, 0, "", proto::err::kProto, "no such player");
    } else if (auto* k = std::get_if<pipe::SeekCmd>(&m)) {
        if (auto it = players_.find(k->id); it != players_.end()) it->second->Seek(*k);
        else Error(k->id, 0, "", proto::err::kProto, "no such player");
    } else if (auto* z = std::get_if<pipe::SizeCmd>(&m)) {
        if (auto it = players_.find(z->id); it != players_.end()) it->second->Size(z->w, z->h);
        else Error(z->id, 0, "", proto::err::kProto, "no such player");
    } else if (auto* cl = std::get_if<pipe::CloseCmd>(&m)) {
        if (auto it = players_.find(cl->id); it != players_.end()) {
            Player::Close(std::move(it->second));
            players_.erase(it);
        }
    } else if (auto* rs = std::get_if<pipe::ResolveCmd>(&m)) {
        resolver_->Start(rs->rid, rs->url);
    } else if (auto* ba = std::get_if<pipe::BtAddCmd>(&m)) {
        OnBtAdd(*ba);
    } else if (auto* bs = std::get_if<pipe::BtSelectCmd>(&m)) {
        std::string err = "this helper has no torrent engine";
        if (!engine_ || !engine_->Select(bs->ih, bs->index, &err)) Error(0, 0, bs->ih, proto::err::kTorrent, err);
    } else if (auto* br = std::get_if<pipe::BtRemoveCmd>(&m)) {
        // The streams go first: players on this torrent stop, any blocked read is released, then the engine removes it.
        for (auto& [id, p] : players_)
            if (p->Ih() == br->ih) p->Stop();
        bridge_->Cancel(br->ih);
        if (engine_) engine_->Remove(br->ih);
        else Error(0, 0, br->ih, proto::err::kTorrent, "this helper has no torrent engine");
    } else if (std::holds_alternative<pipe::QuitCmd>(m)) {
        HLOG("quit");
        quit_ = true;
    }
    // Every other message is helper -> core and never decodes in this direction.
}

void App::OnOpen(const pipe::OpenCmd& c) {
    LoadSpec s;
    s.gen = c.gen;
    s.shm = c.shm;
    if (!c.url.empty()) {
        // Rebuilt from the parsed id once more: mpv only ever sees https://www.youtube.com/watch?v=<id11>.
        url::Classified cl;
        if (!url::Classify(c.url, &cl) || cl.kind != url::Kind::YouTube || !url::ValidVideoId(cl.yt.id)) {
            Error(c.id, c.gen, "", proto::err::kProto, "not a YouTube video link");
            return;
        }
        s.url = url::CanonicalVideoUrl(cl.yt.id);
        s.ytdl = true;
    } else {
        if (!engine_ || !url::ValidInfohash(c.ih) || c.file < 0) {
            Error(c.id, c.gen, c.ih, proto::err::kTorrent, engine_ ? "bad torrent item" : "this helper has no torrent engine");
            return;
        }
        s.url = bt::StreamUrl(c.ih, c.file);
        s.ytdl = false;
        s.ih = c.ih;
    }
    s.start = c.start;
    s.paused = c.paused;
    s.mute = c.mute;
    s.volume = c.volume;
    s.speed = c.speed;
    s.height = c.height;
    s.alang = url::ValidLang(c.alang) ? c.alang : "";
    s.slang = url::ValidLang(c.slang) ? c.slang : "";

    auto it = players_.find(c.id);
    if (it == players_.end()) {
        if (!libmpvError_.empty()) {
            Error(c.id, c.gen, "", proto::err::kLoad, libmpvError_);
            return;
        }
        if (players_.size() >= kMaxPlayers) {
            Error(c.id, c.gen, "", proto::err::kInternal, "too many players");
            return;
        }
        std::string why;
        auto p = Player::Create(c.id, env_, &why);
        if (!p) {
            HLOG("player %u: %s", c.id, why.c_str());
            Error(c.id, c.gen, "", proto::err::kLoad, why);
            return;
        }
        it = players_.emplace(c.id, std::move(p)).first;
    }
    std::string code, why;
    if (!it->second->Load(s, &code, &why)) {
        HLOG("player %u gen %u: %s", c.id, c.gen, why.c_str());
        Error(c.id, c.gen, s.ih, code.c_str(), why);
    }
}

void App::OnBtAdd(const pipe::BtAddCmd& c) {
    bt::Settings st;
    st.cacheDir = args_.cache;
    st.uploadKBps = c.uploadKBps;
    st.upnp = c.upnp;
    st.port = c.port;
    std::string err;
    if (!engine_) {
        engine_ = bt::CreateEngine(st, &err);
        if (!engine_) {
            Error(0, 0, c.ih, proto::err::kTorrent, err.empty() ? "no torrent engine" : err);
            return;
        }
        bridge_->SetEngine(engine_.get());
        HLOG("torrent engine: %s", engine_->Version().c_str());
    }
    if (!engine_->Add(c.ih, c.magnet, st, &err)) Error(0, 0, c.ih, proto::err::kTorrent, err);
}

void App::PollEngine() {
    if (!engine_) return;
    std::vector<bt::Event> evs;
    engine_->Poll(&evs);
    for (auto& e : evs) {
        if (!url::ValidInfohash(e.ih)) continue;
        switch (e.kind) {
            case bt::Event::Kind::Files: {
                pipe::BtFilesEvt f;
                f.ih = e.ih;
                f.name = url::CleanName(e.name, proto::kMaxNameBytes);
                for (const auto& fi : e.files) {
                    if (f.files.size() >= proto::kMaxTorrentFiles) break;
                    if (fi.index < 0 || fi.index >= static_cast<int>(proto::kMaxTorrentFiles)) continue;
                    f.files.push_back({fi.index, url::CleanName(fi.path, proto::kMaxPathBytes), static_cast<double>(fi.size),
                                       fi.video && url::IsVideoFile(fi.path) && !url::IsRefusedFile(fi.path)});
                }
                // A thousand long paths do not fit one line: shorten them, then drop non-video entries.
                for (size_t cap = 256; pipe::Encode(pipe::Message{f}).size() > proto::kMaxLine && cap >= 16; cap /= 2)
                    for (auto& x : f.files) x.path = url::CleanName(x.path, cap);
                if (pipe::Encode(pipe::Message{f}).size() > proto::kMaxLine) std::erase_if(f.files, [](const pipe::BtFile& x) { return !x.video; });
                while (!f.files.empty() && pipe::Encode(pipe::Message{f}).size() > proto::kMaxLine) f.files.pop_back();
                pipe_.Send(pipe::Message{f});
                break;
            }
            case bt::Event::Kind::Status: {
                pipe::BtStatusEvt s;
                s.ih = e.ih;
                s.down = e.status.down < 0 ? 0 : e.status.down;
                s.up = e.status.up < 0 ? 0 : e.status.up;
                s.peers = e.status.peers < 0 ? 0 : e.status.peers > 100000 ? 100000 : e.status.peers;
                s.progress = e.status.progress < 0 ? 0 : e.status.progress > 1 ? 1 : e.status.progress;
                s.have = static_cast<double>(e.status.have);
                s.metadata = e.status.metadata;
                pipe_.Send(pipe::Message{s}, true);
                break;
            }
            case bt::Event::Kind::Removed: pipe_.Send(pipe::Message{pipe::BtRemovedEvt{e.ih}}); break;
            case bt::Event::Kind::Error: {
                const bool disk = e.code == proto::err::kDisk;
                Error(0, 0, e.ih, disk ? proto::err::kDisk : proto::err::kTorrent, e.message);
                break;
            }
        }
    }
}
}  // namespace holo
