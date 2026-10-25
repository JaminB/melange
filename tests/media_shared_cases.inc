// Cases for the media headers shared by the x86 core and the x64 helper (url_logic.h, shm_logic.h, pipe_codec.h),
// included by tests/media_selftest.cpp and helper/holo-media/tests/shared_headers_test.cpp so both architectures run
// the same checks. The includer defines CHECK(cond, what) and includes <atomic>, <chrono>, <string>, <thread>, <vector>.
// Frozen with the headers (Melange 0.12.0 C0).

namespace shared_cases {
namespace url = melange::media::url;
namespace shm = melange::media::shm;
namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;

inline std::string Cls(const std::string& s) {
    url::Classified c;
    return url::Classify(s, &c) ? c.url : std::string("-");
}

inline void Url() {
    const std::string v = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    CHECK(Cls("https://www.youtube.com/watch?v=dQw4w9WgXcQ") == v, "watch link");
    CHECK(Cls("  https://youtube.com/watch?v=dQw4w9WgXcQ&t=42s#x \n") == v, "trimmed, extra params and fragment dropped");
    CHECK(Cls("https://m.youtube.com/watch?feature=share&v=dQw4w9WgXcQ") == v, "m. host, v not first");
    CHECK(Cls("https://music.youtube.com/watch?v=dQw4w9WgXcQ") == v, "music. host");
    CHECK(Cls("HTTPS://WWW.YOUTUBE.COM/watch?v=dQw4w9WgXcQ") == v, "scheme and host are case-insensitive");
    CHECK(Cls("https://youtu.be/dQw4w9WgXcQ?si=abc") == v, "youtu.be");
    CHECK(Cls("https://www.youtube.com/shorts/dQw4w9WgXcQ") == v, "shorts");
    CHECK(Cls("https://www.youtube.com/live/dQw4w9WgXcQ/") == v, "live");
    CHECK(Cls("https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG") ==
              "https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG",
          "playlist");
    url::Classified c;
    CHECK(url::Classify("https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG", &c) &&
              c.url == v && c.yt.playlist == "PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG",
          "video with a list: url is the video, playlist the list");
    CHECK(url::Classify("https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=WL", &c) && c.yt.playlist.empty(), "a too-short list is ignored");
    for (const char* bad : {"http://www.youtube.com/watch?v=dQw4w9WgXcQ", "https://www.youtube.com:443/watch?v=dQw4w9WgXcQ",
                            "https://a@www.youtube.com/watch?v=dQw4w9WgXcQ", "https://evil.com/watch?v=dQw4w9WgXcQ",
                            "https://www.youtube.com.evil.com/watch?v=dQw4w9WgXcQ", "https://www.youtube.com/watch?v=dQw4w9WgXc",
                            "https://www.youtube.com/watch?v=dQw4w9WgXcQQ", "https://www.youtube.com/watch?v=dQw4w9Wg%22cQ",
                            "https://www.youtube.com/embed/dQw4w9WgXcQ", "https://youtu.be/", "file:///C:/x.mp4", "C:\\x.mp4",
                            "javascript:alert(1)", "", "   ", "https://www.youtube.com/watch", "magnet:?dn=x",
                            "magnet:?xt=urn:btih:123", "https://youtu.be/dQw4w9WgXcQ/extra"})
        CHECK(Cls(bad) == "-", std::string("refused: ") + bad);
    CHECK(Cls(v + "&x=" + std::string(4100, 'a')) == "-", "longer than 4096 bytes is refused");

    // Magnets
    const std::string ih = "c9e15763f722f23e98a29decdfae341b98d53056";
    CHECK(Cls("magnet:?xt=urn:btih:C9E15763F722F23E98A29DECDFAE341B98D53056") == "magnet:?xt=urn:btih:" + ih, "hex infohash lowercased");
    CHECK(Cls("magnet:?xt=urn:btih:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == "magnet:?xt=urn:btih:" + std::string(40, '0'), "base32 zeros");
    CHECK(Cls("magnet:?xt=urn:btih:77777777777777777777777777777777") == "magnet:?xt=urn:btih:" + std::string(40, 'f'), "base32 ones");
    CHECK(Cls("magnet:?xt=urn:btmh:1220" + std::string(64, 'A')) == "magnet:?xt=urn:btmh:1220" + std::string(64, 'a'), "v2 btmh");
    CHECK(url::Classify("magnet:?xt=urn:btih:" + ih + "&dn=Big+Buck%20Bunny&tr=udp%3A%2F%2Ftracker.example%3A1337&tr=ftp://x/"
                        "&tr=udp%3A%2F%2Ftracker.example%3A1337&ws=http%3A%2F%2F192.168.0.1%2F&xs=http://x/&x.pe=10.0.0.1:6881&as=http://a/",
                        &c) &&
              c.kind == url::Kind::Magnet && c.bt.name == "Big Buck Bunny" && c.bt.trackers.size() == 1 &&
              c.url == "magnet:?xt=urn:btih:" + ih + "&dn=Big%20Buck%20Bunny&tr=udp%3A%2F%2Ftracker.example%3A1337",
          "magnet: dn decoded, ftp and duplicate trackers dropped, ws/xs/x.pe/as dropped");
    std::string many = "magnet:?xt=urn:btih:" + ih + "&dn=" + std::string(300, 'n');
    for (int i = 0; i < 12; ++i) many += "&tr=https://tracker" + std::to_string(i) + ".example/" + std::string(200, 'p') + "/announce";
    CHECK(url::Classify(many, &c) && c.url.size() <= proto::kMaxMagnetBytes && c.bt.name.size() <= proto::kMaxNameBytes &&
              c.bt.trackers.size() <= proto::kMaxTrackers,
          "a long magnet is cut to the caps and to 1024 bytes");
    CHECK(url::Classify("magnet:?xt=urn:btih:" + ih + "&dn=a%01b%7Fc", &c) && c.bt.name == "abc", "control characters leave dn");

    CHECK(url::ValidLang("en") && url::ValidLang("pt-BR") && url::ValidLang("zh-Hans") && url::ValidLang("deu"), "language codes");
    CHECK(!url::ValidLang("en,exec=calc") && !url::ValidLang("e") && !url::ValidLang("EN") && !url::ValidLang("en-") &&
              !url::ValidLang("en=x") && !url::ValidLang("english"),
          "bad language codes");
    CHECK(url::IsVideoFile("a/b/Movie.MKV") && url::IsVideoFile("x.webm") && !url::IsVideoFile("x.m3u8") && !url::IsVideoFile("mkv"),
          "video file extensions");
    CHECK(url::IsRefusedFile("list.M3U") && url::IsRefusedFile("a.edl") && !url::IsRefusedFile("a.mp4"), "refused file types");
}

inline void ShmRules() {
    shm::SlotMeta m{640, 360, 2560, shm::kFmtBgr0, 1, 0, 0.5};
    CHECK(shm::ValidMeta(m), "a normal frame is valid");
    shm::SlotMeta bad = m;
    bad.w = 100000;
    bad.stride = 400000;
    CHECK(!shm::ValidMeta(bad), "a huge width is refused");
    bad = m;
    bad.stride = 2561;
    CHECK(!shm::ValidMeta(bad), "a stride that is not w*4 is refused");
    bad = m;
    bad.h = 721;
    CHECK(!shm::ValidMeta(bad), "a height over 720 is refused");
    bad = m;
    bad.fmt = 7;
    CHECK(!shm::ValidMeta(bad), "an unknown format is refused");
    bad = m;
    bad.w = 0;
    CHECK(!shm::ValidMeta(bad), "an empty frame is refused");
    CHECK(shm::SectionBytes() == 4096 + 3ull * 1280 * 720 * 4, "section size");

    std::vector<uint8_t> mem(shm::SectionBytes());
    void* base = mem.data();
    shm::InitHeader(base);
    shm::Reader r(base);
    shm::Frame f;
    CHECK(r.Acquire(&f) == shm::ReadResult::None, "nothing published yet");
    shm::Writer w(base);
    CHECK(w.HeaderOk(), "the writer sees the core's header");
    uint32_t s = w.Begin();
    w.Publish(s, m);
    CHECK(r.Acquire(&f) == shm::ReadResult::New && f.slot == s && f.meta.w == 640, "a published frame is read");
    const uint32_t held = f.slot;
    for (int i = 0; i < 5; ++i) {
        const uint32_t n = w.Begin();
        CHECK(n != held, "the writer never picks the slot being read");
        w.Publish(n, m);
    }
    r.Release();
    CHECK(r.Acquire(&f) == shm::ReadResult::New, "the newest frame after release");
    r.Release();
    CHECK(r.Acquire(&f) == shm::ReadResult::Same, "no newer frame: Same");

    // A hostile writer: a bad meta, a bogus latest slot, a broken magic.
    shm::Header* h = static_cast<shm::Header*>(base);
    s = w.Begin();
    shm::SlotMeta evil = m;
    evil.w = 0xFFFFFFFFu;
    evil.stride = 4;
    w.Publish(s, evil);
    const uint64_t before = r.BadFrames();
    CHECK(r.Acquire(&f) == shm::ReadResult::Bad && r.BadFrames() == before + 1, "a hostile meta is skipped and counted");
    CHECK(h->reading == shm::kNone, "a skipped frame holds no slot");
    h->latest = 3u | (99u << 2);
    CHECK(r.Acquire(&f) == shm::ReadResult::None, "latest slot 3 is no frame");
    h->magic = 0;
    h->latest = 0u | (100u << 2);
    CHECK(r.Acquire(&f) == shm::ReadResult::Bad, "a broken magic is refused");
}

// A writer thread fills each frame with one byte value; the reader checks every frame it holds is uniform. A torn
// read (the writer reusing a held slot) shows as a mixed frame.
inline void ShmThreads() {
    std::vector<uint8_t> mem(shm::SectionBytes());
    void* base = mem.data();
    shm::InitHeader(base);
    std::atomic<bool> stop{false};
    std::thread wt([&] {
        shm::Writer w(base);
        uint32_t n = 0;
        while (!stop.load()) {
            const uint32_t s = w.Begin();
            const uint8_t v = static_cast<uint8_t>(++n);
            uint8_t* d = w.Data(s);
            for (size_t i = 0; i < 64 * 64 * 4; ++i) d[i] = v;
            w.Publish(s, shm::SlotMeta{64, 64, 256, shm::kFmtBgr0, n, 0, 0});
        }
    });
    shm::Reader r(base);
    int frames = 0, torn = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (frames < 3000 && std::chrono::steady_clock::now() < until) {
        shm::Frame f;
        if (r.Acquire(&f) != shm::ReadResult::New) continue;
        ++frames;
        const uint8_t v = f.data[0];
        for (size_t k = 0; k < 64 * 64 * 4; k += 61)
            if (f.data[k] != v) {
                ++torn;
                break;
            }
        r.Release();
    }
    stop = true;
    wt.join();
    CHECK(frames > 0, "the reader saw frames from the writer thread");
    CHECK(torn == 0, "no torn frame across " + std::to_string(frames) + " reads");
}

inline bool RoundTrip(const pipe::Message& m, pipe::Dir d) {
    const std::string line = pipe::Encode(m);
    if (line.empty() || line.back() != '\n') return false;
    pipe::Message back;
    std::string why;
    if (!pipe::Decode(std::string_view(line).substr(0, line.size() - 1), d, &back, &why)) {
        printf("  round trip failed: %s (%s)\n", line.c_str(), why.c_str());
        return false;
    }
    return pipe::Encode(back) == line && back.index() == m.index();
}

inline bool Dec(const std::string& line, pipe::Dir d, pipe::Message* out = nullptr) {
    pipe::Message m;
    return pipe::Decode(line, d, out ? out : &m);
}

inline void Pipe() {
    using D = pipe::Dir;
    const std::string ih(40, 'a');
    pipe::OpenCmd o;
    o.id = 1;
    o.gen = 3;
    o.shm = "Local\\melange-holo-1234-00ff-1";
    o.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    o.start = 12.5;
    o.alang = "en";
    pipe::OpenCmd ob = o;
    ob.url.clear();
    ob.ih = ih;
    ob.file = 2;
    pipe::SetCmd sb{1, "pause", true, true, 0}, sn{1, "volume", false, false, 55.5}, so{1, "sid", true, false, 0};
    pipe::PropEvt pt;
    pt.id = 1;
    pt.name = "time-pos";
    pt.kind = pipe::PropEvt::Kind::Number;
    pt.number = 3.25;
    pt.qpcMs = 123456.5;
    pipe::PropEvt pl;
    pl.id = 1;
    pl.name = "track-list";
    pl.kind = pipe::PropEvt::Kind::Tracks;
    pl.tracks = {{1, "audio", "en", "English", true}, {2, "sub", "de", "Deutsch \"forced\"", false}};
    pipe::PropEvt pv;
    pv.id = 1;
    pv.name = "video-params";
    pv.kind = pipe::PropEvt::Kind::Video;
    pv.video = {1920, 1080, 1.7777777777777777};
    pipe::PropEvt pn;
    pn.id = 1;
    pn.name = "duration";
    pipe::ResolvedEvt rs;
    rs.rid = 9;
    rs.title = "Mix";
    rs.items = {{"https://www.youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ", "Never", 212}};
    pipe::ResolvedEvt re;
    re.rid = 9;
    re.error = "private";
    re.message = "Video unavailable";
    pipe::BtFilesEvt bf{ih, "Sintel", {{0, "Sintel/sintel.mp4", 129241752, true}, {1, "Sintel/sintel.srt", 1652, false}}};
    pipe::BtStatusEvt bs{ih, 1.5e6, 2e4, 17, 0.25, 32e6, true};

    CHECK(RoundTrip(pipe::HelloCmd{}, D::ToHelper), "hello (core)");
    CHECK(RoundTrip(o, D::ToHelper), "open url");
    CHECK(RoundTrip(ob, D::ToHelper), "open bt");
    CHECK(RoundTrip(sb, D::ToHelper) && RoundTrip(sn, D::ToHelper) && RoundTrip(so, D::ToHelper), "set");
    CHECK(RoundTrip(pipe::SeekCmd{1, 30.25, "keyframes"}, D::ToHelper) && RoundTrip(pipe::SeekCmd{1, -10, "relative"}, D::ToHelper), "seek");
    CHECK(RoundTrip(pipe::SizeCmd{1, 960, 540}, D::ToHelper) && RoundTrip(pipe::CloseCmd{1}, D::ToHelper), "size, close");
    CHECK(RoundTrip(pipe::ResolveCmd{4, "https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG"}, D::ToHelper), "resolve");
    CHECK(RoundTrip(pipe::BtAddCmd{ih, "magnet:?xt=urn:btih:" + ih, 512, false, 0}, D::ToHelper), "bt.add");
    CHECK(RoundTrip(pipe::BtSelectCmd{ih, 3}, D::ToHelper) && RoundTrip(pipe::BtRemoveCmd{ih}, D::ToHelper), "bt.select, bt.remove");
    CHECK(RoundTrip(pipe::QuitCmd{}, D::ToHelper) && RoundTrip(pipe::PingCmd{}, D::ToHelper), "quit, ping");
    CHECK(RoundTrip(pipe::HelloEvt{1, "mpv v0.40", "n7.1", "2.0.11", "d3d11va"}, D::ToCore), "hello (helper)");
    CHECK(RoundTrip(pt, D::ToCore) && RoundTrip(pl, D::ToCore) && RoundTrip(pv, D::ToCore) && RoundTrip(pn, D::ToCore), "props");
    CHECK(RoundTrip(pipe::EventEvt{1, 3, "end-file", "eof"}, D::ToCore), "event");
    CHECK(RoundTrip(pipe::ErrorEvt{1, 3, 0, "", "load", "no such file"}, D::ToCore), "error");
    CHECK(RoundTrip(rs, D::ToCore) && RoundTrip(re, D::ToCore), "resolved");
    CHECK(RoundTrip(bf, D::ToCore) && RoundTrip(bs, D::ToCore) && RoundTrip(pipe::BtRemovedEvt{ih}, D::ToCore), "bt events");
    CHECK(RoundTrip(pipe::PongEvt{}, D::ToCore), "pong");

    // Direction: a helper line is not a command, and the other way round.
    CHECK(!Dec(R"({"t":"pong"})", D::ToHelper) && !Dec(R"({"t":"ping"})", D::ToCore), "direction is enforced");
    CHECK(!Dec(R"({"t":"exec","cmd":"calc"})", D::ToHelper), "unknown messages are refused");
    // Hostile values.
    CHECK(!Dec(R"({"t":"open","id":1,"gen":0,"shm":"x","url":"https://evil.com/watch?v=dQw4w9WgXcQ"})", D::ToHelper), "open: a foreign url");
    CHECK(!Dec(R"({"t":"open","id":1,"gen":0,"shm":"a b","url":"https://youtu.be/dQw4w9WgXcQ"})", D::ToHelper), "open: a bad section name");
    pipe::Message m;
    CHECK(Dec(R"({"t":"open","id":1,"gen":0,"shm":"x","url":" https://youtu.be/dQw4w9WgXcQ?x=1","slang":"en,exec=calc","alang":"en"})",
              D::ToHelper, &m) &&
              std::get<pipe::OpenCmd>(m).slang.empty() && std::get<pipe::OpenCmd>(m).alang == "en" &&
              std::get<pipe::OpenCmd>(m).url == "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
          "open: a crafted language is dropped and the url rebuilt");
    CHECK(!Dec(R"({"t":"open","id":1,"gen":0,"shm":"x","url":"https://youtu.be/dQw4w9WgXcQ","bt":{"ih":"aa","index":0}})", D::ToHelper),
          "open: both sources");
    CHECK(!Dec(R"({"t":"set","id":1,"prop":"input-ipc-server","value":"x"})", D::ToHelper), "set: an unknown property");
    CHECK(!Dec(R"({"t":"set","id":1,"prop":"volume","value":1000})", D::ToHelper), "set: volume out of range");
    CHECK(!Dec(R"({"t":"seek","id":1,"pos":-5,"mode":"exact"})", D::ToHelper), "seek: a negative absolute time");
    CHECK(!Dec(R"({"t":"bt.add","ih":")" + ih + R"(","magnet":"magnet:?xt=urn:btih:)" + std::string(40, 'b') + R"("})", D::ToHelper),
          "bt.add: magnet and ih disagree");
    CHECK(!Dec(R"({"t":"prop","id":1,"gen":0,"name":"time-pos","value":1e300})", D::ToCore), "prop: a number out of range");
    CHECK(!Dec(R"({"t":"prop","id":1,"gen":0,"name":"time-pos","value":"x"})", D::ToCore), "prop: a wrong type");
    CHECK(!Dec(R"({"t":"prop","id":1,"gen":0,"name":"secret","value":1})", D::ToCore), "prop: an unknown name");
    CHECK(!Dec(R"({"t":"bt.status","ih":"../../x"})", D::ToCore), "bt.status: a bad infohash");
    CHECK(Dec(R"({"t":"resolved","rid":2,"items":[{"url":"https://evil.com/","id":"dQw4w9WgXcQ","title":"a"},{"id":"bad","title":"b"}]})",
              D::ToCore, &m) &&
              std::get<pipe::ResolvedEvt>(m).items.size() == 1 &&
              std::get<pipe::ResolvedEvt>(m).items[0].url == "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
          "resolved: urls are rebuilt from ids and bad items skipped");
    CHECK(Dec(R"({"t":"prop","id":1,"gen":0,"name":"media-title","value":"a\u0001b\ud83d\ude00"})", D::ToCore, &m) &&
              std::get<pipe::PropEvt>(m).str == "ab\xF0\x9F\x98\x80",
          "a title loses control characters and keeps a surrogate pair");
    CHECK(!Dec(R"({"t":"pong","x":"\ud800"})", D::ToCore), "a lone surrogate is malformed");
    CHECK(!Dec(R"({"t":"pong","t":"pong"})", D::ToCore), "duplicate keys are malformed");
    std::string deep = R"({"t":"pong","x":)";
    for (int i = 0; i < 20; ++i) deep += "[";
    for (int i = 0; i < 20; ++i) deep += "]";
    CHECK(!Dec(deep + "}", D::ToCore), "nesting deeper than kMaxDepth is refused");
    CHECK(!Dec(R"({"t":"pong","x":")" + std::string(proto::kMaxLine, 'a') + R"("})", D::ToCore), "a line over 64 KB is refused");
    CHECK(!Dec("[1,2]", D::ToCore) && !Dec("", D::ToCore) && !Dec("{", D::ToCore) && !Dec(R"({"t":1})", D::ToCore), "malformed lines");

    // Line splitting.
    pipe::LineSplitter sp;
    std::vector<std::string> got;
    const std::string stream = "{\"t\":\"pong\"}\r\n\n" + std::string(proto::kMaxLine + 10, 'x') + "\n{\"t\":\"pong\"}\n{\"t\":";
    for (size_t i = 0; i < stream.size(); i += 7)
        sp.Feed(stream.data() + i, std::min<size_t>(7, stream.size() - i), [&](std::string_view l) { got.emplace_back(l); });
    CHECK(got.size() == 2 && got[0] == "{\"t\":\"pong\"}" && got[1] == got[0], "lines split across reads, CR stripped, empty lines skipped");
    CHECK(sp.Dropped() == 1 && sp.Pending() == 5, "an oversized line is dropped whole; a partial line waits");

    // Mutation fuzz: no input may crash the decoder or make it accept a value outside the caps.
    const std::string seeds[] = {pipe::Encode(o), pipe::Encode(pl), pipe::Encode(rs), pipe::Encode(bf), pipe::Encode(bs), pipe::Encode(sn)};
    uint32_t rng = 12345;
    auto next = [&] { return rng = rng * 1664525u + 1013904223u; };
    const char alphabet[] = "{}[]\":,\\u0123456789abcdefnull-+.eE truefalse\x01\xff";
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string s = seeds[next() % 6];
        s.pop_back();
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int k = 0; k < edits && !s.empty(); ++k) {
            const size_t at = next() % s.size();
            switch (next() % 3) {
                case 0: s[at] = alphabet[next() % (sizeof alphabet - 1)]; break;
                case 1: s.erase(at, 1 + next() % 8); break;
                default: s.insert(at, 1, alphabet[next() % (sizeof alphabet - 1)]); break;
            }
        }
        for (pipe::Dir d : {D::ToHelper, D::ToCore}) {
            pipe::Message out;
            if (!pipe::Decode(s, d, &out)) continue;
            ++accepted;
            if (auto* p = std::get_if<pipe::OpenCmd>(&out)) {
                CHECK(p->volume >= 0 && p->volume <= 100 && p->speed >= 0.25 && p->speed <= 4 && (p->slang.empty() || url::ValidLang(p->slang)),
                      "fuzz: an accepted open stays in range");
            }
            if (auto* p = std::get_if<pipe::BtFilesEvt>(&out)) {
                CHECK(url::ValidInfohash(p->ih) && p->files.size() <= proto::kMaxTorrentFiles, "fuzz: accepted bt.files stays in range");
            }
        }
    }
    CHECK(accepted > 0, "fuzz: some mutations still decode (the run covers the accept path too)");
}

inline void All() {
    Url();
    ShmRules();
    ShmThreads();
    Pipe();
}
}  // namespace shared_cases
