// A stand-in for holo-media.exe in media_selftest (x86, no mpv, no libtorrent): the same command line, the same pipe
// protocol and the same frame section, with synthetic frames and scripted faults, so the media host is tested without
// the real helper (design 4, Critique D2). It plays a fake 120 s item at the requested speed, answers resolve with a
// fixed list and bt.add with a two-file torrent.
//
// Extra flags (tests only; the real helper rejects them): --fault <name> with name one of
//   hang      after the first open: stop reading and stop answering pings (the watchdog must kill it)
//   crash     300 ms after hello: terminate with an access-violation code
//   hostile   frames carry hostile slot metadata (huge w/h, wrong stride, unknown format)
//   oversize  a 70 KB line before hello (dropped whole), then normal
//   garbage   malformed and out-of-range lines before hello, then normal
// and --probe-deny <dir>: try to create a file there and in the cache, and write the outcome to <cache>\probe.txt
// ("cache=ok|failed deny=denied|written"), so the test can see the integrity level at work.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/pipe_codec.h"
#include "media/shm_logic.h"

namespace {
namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;
namespace shm = melange::media::shm;

HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::mutex g_writeMu;
std::mutex g_inMu;
std::deque<pipe::Message> g_in;
std::atomic<bool> g_hangNow{false};
std::wstring g_fault;

double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

bool IoWait(OVERLAPPED* ov, DWORD* n) { return GetOverlappedResult(g_pipe, ov, n, TRUE) != FALSE; }

void WriteRaw(const std::string& s) {
    std::lock_guard lk(g_writeMu);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    size_t off = 0;
    while (off < s.size()) {
        DWORD n = 0;
        ResetEvent(ov.hEvent);
        if (!WriteFile(g_pipe, s.data() + off, static_cast<DWORD>(s.size() - off), &n, &ov) &&
            (GetLastError() != ERROR_IO_PENDING || !IoWait(&ov, &n)))
            ExitProcess(2);  // the core closed the pipe
        off += n;
    }
    CloseHandle(ov.hEvent);
}
void Send(const pipe::Message& m) { WriteRaw(pipe::Encode(m)); }

void ReaderThread() {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pipe::LineSplitter split;
    std::vector<char> buf(65536);
    for (;;) {
        while (g_hangNow.load()) Sleep(1000);
        DWORD n = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(g_pipe, buf.data(), static_cast<DWORD>(buf.size()), &n, &ov) && (GetLastError() != ERROR_IO_PENDING || !IoWait(&ov, &n)))
            ExitProcess(0);
        if (n == 0) ExitProcess(0);
        split.Feed(buf.data(), n, [](std::string_view line) {
            pipe::Message m;
            std::string why;
            if (!pipe::Decode(line, pipe::Dir::ToHelper, &m, &why)) {
                pipe::ErrorEvt e;
                e.code = proto::err::kProto;
                e.message = why;
                Send(e);
                return;
            }
            if (std::holds_alternative<pipe::PingCmd>(m)) {
                if (!g_hangNow.load()) Send(pipe::PongEvt{});
                return;
            }
            if (std::holds_alternative<pipe::QuitCmd>(m)) ExitProcess(0);
            std::lock_guard lk(g_inMu);
            g_in.push_back(std::move(m));
        });
    }
}

struct Player {
    uint32_t id = 0, gen = 0;
    HANDLE map = nullptr;
    void* base = nullptr;
    shm::Writer* writer = nullptr;
    int w = 320, h = 180;
    double pos = 0, speed = 1, duration = 120;
    bool paused = true, eof = false;
    double bufferUntil = 0;  // simulated paused-for-cache after a seek
    bool buffering = false;
    int sid = 2;
    double nextFrame = 0, nextPos = 0;
    uint32_t frameNo = 0;
};

pipe::PropEvt Prop(const Player& p, const char* name) {
    pipe::PropEvt e;
    e.id = p.id;
    e.gen = p.gen;
    e.name = name;
    return e;
}
void SendNum(const Player& p, const char* name, double v, double qpc = 0) {
    pipe::PropEvt e = Prop(p, name);
    e.kind = pipe::PropEvt::Kind::Number;
    e.number = v;
    e.qpcMs = qpc;
    Send(e);
}
void SendBool(const Player& p, const char* name, bool v) {
    pipe::PropEvt e = Prop(p, name);
    e.kind = pipe::PropEvt::Kind::Bool;
    e.boolean = v;
    Send(e);
}
void SendTracks(const Player& p) {
    pipe::PropEvt e = Prop(p, proto::prop::kTrackList);
    e.kind = pipe::PropEvt::Kind::Tracks;
    e.tracks = {{1, "video", "", "", true}, {1, "audio", "en", "English", true}, {2, "sub", "en", "English", p.sid == 2},
                {3, "sub", "de", "Deutsch", p.sid == 3}};
    Send(e);
}

void Unmap(Player& p) {
    delete p.writer;
    p.writer = nullptr;
    if (p.base) UnmapViewOfFile(p.base);
    if (p.map) CloseHandle(p.map);
    p.base = p.map = nullptr;
}

void Render(Player& p) {
    if (!p.writer) return;
    const uint32_t slot = p.writer->Begin();
    uint8_t* d = p.writer->Data(slot);
    const uint8_t shade = static_cast<uint8_t>(p.frameNo * 7);
    for (int y = 0; y < p.h; ++y) {
        uint8_t* row = d + static_cast<size_t>(y) * p.w * 4;
        for (int x = 0; x < p.w; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>(x);
            row[x * 4 + 1] = static_cast<uint8_t>(y);
            row[x * 4 + 2] = shade;
            row[x * 4 + 3] = 0;
        }
    }
    shm::SlotMeta m{};
    m.w = static_cast<uint32_t>(p.w);
    m.h = static_cast<uint32_t>(p.h);
    m.stride = m.w * 4;
    m.fmt = shm::kFmtBgr0;
    m.frameNo = ++p.frameNo;
    m.pts = p.pos;
    if (g_fault == L"hostile") {
        switch (p.frameNo % 4) {
            case 0: m.w = 99999; m.stride = m.w * 4; break;
            case 1: m.h = 0x7FFFFFFF; break;
            case 2: m.stride = 1; break;
            default: m.fmt = 77; break;
        }
    }
    p.writer->Publish(slot, m);
}

void Open(std::map<uint32_t, Player>& players, const pipe::OpenCmd& c) {
    Player& p = players[c.id];
    Unmap(p);
    p.id = c.id;
    p.gen = c.gen;
    std::wstring wname(c.shm.begin(), c.shm.end());
    p.map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, wname.c_str());
    if (p.map) p.base = MapViewOfFile(p.map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, shm::SectionBytes());
    if (!p.base) {
        pipe::ErrorEvt e;
        e.id = c.id;
        e.gen = c.gen;
        e.code = proto::err::kShm;
        e.message = "could not map " + c.shm + " (" + std::to_string(GetLastError()) + ")";
        Send(e);
        Unmap(p);
        return;
    }
    p.writer = new shm::Writer(p.base);
    p.pos = c.start;
    p.paused = c.paused;
    p.speed = c.speed;
    p.eof = false;
    pipe::PropEvt t = Prop(p, proto::prop::kMediaTitle);
    t.kind = pipe::PropEvt::Kind::String;
    t.str = c.url.empty() ? "Fake torrent file " + std::to_string(c.file) : "Fake video";
    Send(t);
    SendNum(p, proto::prop::kDuration, p.duration);
    SendTracks(p);
    pipe::PropEvt v = Prop(p, proto::prop::kVideoParams);
    v.kind = pipe::PropEvt::Kind::Video;
    v.video = {1920, 1080, 16.0 / 9.0};
    Send(v);
    SendBool(p, proto::prop::kPause, p.paused);
    SendBool(p, proto::prop::kPausedForCache, false);
    SendNum(p, proto::prop::kSpeed, p.speed);
    pipe::EventEvt ev;
    ev.id = p.id;
    ev.gen = p.gen;
    ev.name = proto::event::kFileLoaded;
    Send(ev);
    SendNum(p, proto::prop::kTimePos, p.pos, NowMs());
    if (g_fault == L"hang") g_hangNow.store(true);
}

bool g_parentOk = false;

void Probe(const std::wstring& cache, const std::wstring& deny) {
    auto tryWrite = [](const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
    };
    const bool cacheOk = tryWrite(cache + L"\\tmp\\probe-write.tmp");
    const bool denyWritten = tryWrite(deny + L"\\fake-helper-probe.tmp");
    std::string out = std::string("cache=") + (cacheOk ? "ok" : "failed") + " deny=" + (denyWritten ? "written" : "denied") +
                            "\nparent=" + (g_parentOk ? "ok" : "denied");
    HANDLE h = CreateFileW((cache + L"\\probe.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD n = 0;
        WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &n, nullptr);
        CloseHandle(h);
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring pipeName, cache, deny;
    DWORD parent = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == proto::arg::kPipe && hasValue) pipeName = argv[++i];
        else if (a == proto::arg::kParent && hasValue) parent = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (a == proto::arg::kCache && hasValue) cache = argv[++i];
        else if ((a == proto::arg::kYtdlp || a == proto::arg::kDeno || a == proto::arg::kLibmpv) && hasValue) ++i;
        else if (a == proto::arg::kSweep) continue;
        else if (a == L"--fault" && hasValue) g_fault = argv[++i];
        else if (a == L"--probe-deny" && hasValue) deny = argv[++i];
        else return 64;  // the real helper refuses anything it does not know too
    }
    if (pipeName.empty() || !parent) return 64;
    if (HANDLE par = OpenProcess(SYNCHRONIZE, FALSE, parent)) {
        g_parentOk = true;
        std::thread([par] {
            WaitForSingleObject(par, INFINITE);
            ExitProcess(0);  // the game went away
        }).detach();
    }
    if (!deny.empty() && !cache.empty()) Probe(cache, deny);
    g_pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (g_pipe == INVALID_HANDLE_VALUE) return 3;

    if (g_fault == L"oversize") WriteRaw(std::string(70000, 'x') + "\n");
    if (g_fault == L"garbage") {
        WriteRaw("{not json\n{\"t\":\"nope\"}\n{\"t\":\"prop\",\"id\":1,\"gen\":0,\"name\":\"time-pos\",\"value\":1e300}\n");
        WriteRaw("{\"t\":\"bt.files\",\"ih\":\"../../x\",\"files\":[]}\n[1,2,3]\n\"str\"\n{\"t\":\"event\",\"id\":1,\"gen\":0,\"name\":\"rm -rf\"}\n");
    }
    pipe::HelloEvt hello;
    hello.mpv = "fake";
    hello.ffmpeg = "fake";
    hello.libtorrent = "fake";
    hello.hwdec = "no";
    Send(hello);
    const double crashAt = g_fault == L"crash" ? NowMs() + 300 : 0;
    std::thread(ReaderThread).detach();

    std::map<uint32_t, Player> players;
    struct Torrent {
        double filesAt = 0, nextStatus = 0;
        bool filesSent = false;
        int selected = -1;
    };
    std::map<std::string, Torrent> torrents;
    double last = NowMs();
    for (;;) {
        Sleep(5);
        while (g_hangNow.load()) Sleep(1000);  // a hung helper: nothing moves, no pongs, nothing read
        const double now = NowMs();
        const double dt = (now - last) / 1000.0;
        last = now;
        if (crashAt && now >= crashAt) TerminateProcess(GetCurrentProcess(), 0xC0000005);
        std::deque<pipe::Message> in;
        {
            std::lock_guard lk(g_inMu);
            in.swap(g_in);
        }
        for (pipe::Message& m : in) {
            if (auto* o = std::get_if<pipe::OpenCmd>(&m)) {
                Open(players, *o);
            } else if (auto* s = std::get_if<pipe::SetCmd>(&m)) {
                auto it = players.find(s->id);
                if (it == players.end()) continue;
                Player& p = it->second;
                namespace sp = proto::setprop;
                if (s->prop == sp::kPause) {
                    p.paused = s->boolean;
                    SendBool(p, proto::prop::kPause, p.paused);
                    SendNum(p, proto::prop::kTimePos, p.pos, now);
                } else if (s->prop == sp::kSpeed) {
                    p.speed = s->number;
                    SendNum(p, proto::prop::kSpeed, p.speed);
                } else if (s->prop == sp::kSid) {
                    p.sid = s->isBool ? 0 : static_cast<int>(s->number);
                    SendTracks(p);
                }
            } else if (auto* k = std::get_if<pipe::SeekCmd>(&m)) {
                auto it = players.find(k->id);
                if (it == players.end()) continue;
                Player& p = it->second;
                p.pos = std::clamp(k->mode == "relative" ? p.pos + k->t : k->t, 0.0, p.duration);
                p.eof = false;
                p.buffering = true;
                p.bufferUntil = now + 150;
                SendBool(p, proto::prop::kPausedForCache, true);
            } else if (auto* z = std::get_if<pipe::SizeCmd>(&m)) {
                auto it = players.find(z->id);
                if (it == players.end()) continue;
                it->second.w = z->w;
                it->second.h = z->h;
            } else if (auto* c = std::get_if<pipe::CloseCmd>(&m)) {
                auto it = players.find(c->id);
                if (it == players.end()) continue;
                pipe::EventEvt ev;
                ev.id = c->id;
                ev.gen = it->second.gen;
                ev.name = proto::event::kEndFile;
                ev.reason = "stop";
                Send(ev);
                Unmap(it->second);
                players.erase(it);
            } else if (auto* r = std::get_if<pipe::ResolveCmd>(&m)) {
                pipe::ResolvedEvt e;
                e.rid = r->rid;
                if (r->url.find("list=PLprivate") != std::string::npos) {
                    e.error = proto::err::kPrivate;
                    e.message = "This playlist is private";
                } else if (r->url.find("list=") != std::string::npos) {
                    e.title = "Fake list";
                    e.items = {{"", "aaaaaaaaaaa", "First", 61}, {"", "bbbbbbbbbbb", "Second", 0}, {"", "ccccccccccc", "Third", 3600}};
                } else {
                    e.title = "Fake video";
                    e.items = {{"", r->url.substr(r->url.size() - 11), "Fake video", 120}};
                }
                Send(e);
            } else if (auto* b = std::get_if<pipe::BtAddCmd>(&m)) {
                torrents[b->ih].filesAt = now + 200;
            } else if (auto* bs = std::get_if<pipe::BtSelectCmd>(&m)) {
                auto it = torrents.find(bs->ih);
                if (it != torrents.end()) it->second.selected = bs->index;
            } else if (auto* br = std::get_if<pipe::BtRemoveCmd>(&m)) {
                torrents.erase(br->ih);
                pipe::BtRemovedEvt e;
                e.ih = br->ih;
                Send(e);
            }
        }
        for (auto& [ih, t] : torrents) {
            if (!t.filesSent && now >= t.filesAt) {
                pipe::BtFilesEvt e;
                e.ih = ih;
                e.name = "Fake torrent";
                e.files = {{0, "Fake/movie.mkv", 100e6, true}, {1, "Fake/readme.txt", 100, false}};
                Send(e);
                t.filesSent = true;
            }
            if (t.filesSent && now >= t.nextStatus) {
                pipe::BtStatusEvt e;
                e.ih = ih;
                e.down = 1e6;
                e.up = 2e5;
                e.peers = 7;
                e.progress = t.selected >= 0 ? 0.25 : 0;
                e.have = t.selected >= 0 ? 25e6 : 0;
                e.metadata = true;
                Send(e);
                t.nextStatus = now + 1000;
            }
        }
        for (auto& [id, p] : players) {
            if (p.buffering && now >= p.bufferUntil) {
                p.buffering = false;
                SendBool(p, proto::prop::kPausedForCache, false);
                SendNum(p, proto::prop::kTimePos, p.pos, now);
            }
            if (!p.paused && !p.buffering && !p.eof) {
                p.pos += dt * p.speed;
                if (p.pos >= p.duration) {
                    p.pos = p.duration;
                    p.eof = true;
                    SendBool(p, proto::prop::kEofReached, true);
                }
            }
            if (now >= p.nextPos) {
                SendNum(p, proto::prop::kTimePos, p.pos, now);
                SendNum(p, proto::prop::kCacheTime, std::min(p.duration, p.pos + 20));
                p.nextPos = now + 1000.0 / proto::kTimePosHz;
            }
            if (now >= p.nextFrame) {
                Render(p);
                p.nextFrame = now + 33;
            }
        }
    }
}
