// sandbox_selftest cases for wum.media (owner: C4b). Included in the test file's anonymous namespace after the helpers
// (SetMod, Eval, Frames, Expect, ExpectEq, StatusOf); defines TestMedia(), run before the docs check. The media host
// and the pack are fakes (sandbox_fakes_media.inc), so these cases cover the binding: argument types, handle
// ownership and lifetime, the install gesture, callbacks and owner-only events.
void TestMedia() {
    namespace fm = fake_media;
    using media::pack::State;
    auto starts = [](const std::string& s, const char* p) { return s.rfind(p, 0) == 0; };
    ExpectEq(Eval(nullptr, "return type(wum.media)"), "table", "wum.media exists");
    ExpectEq(Eval(nullptr, "local c = wum.media.classify(' https://youtu.be/dQw4w9WgXcQ?t=3 ') return c.kind .. ' ' .. c.id .. ' ' .. c.url"),
             "youtube dQw4w9WgXcQ https://www.youtube.com/watch?v=dQw4w9WgXcQ", "classify: a youtu.be link");
    ExpectEq(Eval(nullptr, "local c = wum.media.classify('magnet:?xt=urn:btih:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA&ws=http://x/') "
                           "return c.kind .. ' ' .. c.infohash .. ' ' .. c.url"),
             "magnet 0000000000000000000000000000000000000000 magnet:?xt=urn:btih:0000000000000000000000000000000000000000",
             "classify: a base32 magnet, ws dropped");
    ExpectEq(Eval(nullptr, "local a, b = wum.media.classify('http://youtu.be/dQw4w9WgXcQ') return tostring(a) .. ':' .. b"),
             "nil:unsupported-url", "classify: http is refused");
    Expect(starts(Eval(nullptr, "return pcall(wum.media.classify, {})"), "false"), "classify: a non-string raises");
    ExpectEq(Eval(nullptr, "return tostring(wum.media.available())"), "true", "available() with the host up");
    fm::g_available = false;
    fm::g_reason = "unsupported-os";
    ExpectEq(Eval(nullptr, "local a, b = wum.media.available() return tostring(a) .. ':' .. tostring(b)"), "false:unsupported-os",
             "available() passes the host's reason");
    fm::g_available = true;
    Expect(Eval(nullptr, "return pcall(wum.media.open, {url = 'https://youtu.be/dQw4w9WgXcQ'})").find("mod context") != std::string::npos,
           "open needs a mod");

    SetMod("med", "evs = {} wum.events.on('melange.media', function(e) evs[#evs + 1] = e.type .. ':' .. tostring(e.title or e.state or '') end)");
    SetMod("med2", "evs = {} wum.events.on('melange.media', function(e) evs[#evs + 1] = e.type end)");
    Expect(sandbox::LoadMod("med") && sandbox::LoadMod("med2"), "media mods load");

    // ---- the pack and the install gesture
    ExpectEq(Eval("med", "local p = wum.media.pack() return p.state .. ' ' .. p.version .. ' ' .. p.downloadMB .. ' ' .. p.diskMB"),
             "missing 1 95 280", "pack(): state, version and the consent sizes");
    Expect(fm::g_touched.count("med") == 1, "pack() subscribes the mod to pack events");
    ExpectEq(Eval("med", "local a, b = wum.media.install() return tostring(a) .. ':' .. tostring(b)"), "nil:gesture", "install without a click");
    input::gesture::Mark("med", false);
    ExpectEq(Eval("med", "local a, b = wum.media.install() return tostring(a) .. ':' .. tostring(b)"), "nil:gesture",
             "install after a click that is not consent-grade (a big or new region)");
    input::gesture::Mark("med2", true);
    ExpectEq(Eval("med", "local a, b = wum.media.install() return tostring(a) .. ':' .. tostring(b)"), "nil:gesture",
             "another mod's click is not this mod's consent");
    input::gesture::Mark("med", true);
    ExpectEq(Eval("med", "return tostring(wum.media.install())"), "true", "install right after a consent-grade click");
    Expect(fm::g_installs == 1 && fm::g_installMod == "med", "pack::Install called once, for the clicking mod");
    ExpectEq(Eval("med", "local a, b = wum.media.install() return tostring(a) .. ':' .. tostring(b)"), "nil:gesture", "one click, one install");
    ExpectEq(Eval("med", "return tostring(wum.media.cancelInstall())"), "true", "cancelInstall");
    fm::g_packState = State::Ready;
    ExpectEq(Eval("med", "local a, b = wum.media.install() return tostring(a) .. ':' .. tostring(b)"), "nil:ready", "install when ready");
    fm::g_packState = State::Error;
    ExpectEq(Eval("med", "local p = wum.media.pack() return p.state .. ':' .. tostring(p.error)"), "error:hash", "pack(): the error reason");
    ExpectEq(Eval("med", "local a, b = wum.media.open{url = 'https://youtu.be/dQw4w9WgXcQ'} return tostring(a) .. ':' .. tostring(b)"),
             "nil:pack", "open before the pack is ready");
    fm::g_packState = State::Ready;

    // ---- open: arguments
    ExpectEq(Eval("med", "local a, b = wum.media.open{url = 'https://example.com/v.mp4'} return tostring(a) .. ':' .. b"),
             "nil:unsupported-url", "open: another host");
    ExpectEq(Eval("med", "local a, b = wum.media.open{} return tostring(a) .. ':' .. b"), "nil:unsupported-url", "open: no source");
    ExpectEq(Eval("med", "local a, b = wum.media.open{url = 'https://www.youtube.com/playlist?list=PL0123456789ab'} "
                         "return tostring(a) .. ':' .. b"),
             "nil:unsupported-url", "open: a playlist is resolved, not played");
    Expect(starts(Eval("med", "return pcall(wum.media.open, {url = 'https://youtu.be/dQw4w9WgXcQ', start = 'x'})"), "false"),
           "open: a wrong option type raises");
    Expect(starts(Eval("med", "return pcall(wum.media.open, {torrent = {}})"), "false"), "open: a fake torrent raises");
    Expect(starts(Eval("med", "return pcall(wum.media.open, 'x')"), "false"), "open: opts must be a table");

    ExpectEq(Eval("med", "P = wum.media.open{url = 'https://m.youtube.com/watch?v=dQw4w9WgXcQ&list=PL0123456789ab', start = 4, "
                         "audioLang = 'en,exec=calc', subLang = 'pt-BR', quality = 1080, speed = 9} return type(P) .. ' ' .. tostring(P)"),
             "userdata media player", "open returns a player");
    Expect(fm::g_players.size() == 1, "the host has one player");
    if (!fm::g_players.empty()) {
        const fm::Player& p = fm::g_players.begin()->second;
        ExpectEq(p.spec.url, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "open: the canonical video URL reaches the host");
        Expect(p.spec.start == 4 && p.spec.paused && p.spec.volume == 70 && p.spec.quality == 1080 && p.spec.speed == 4,
               "open: start, default pause and volume, quality, speed clamped");
        Expect(p.spec.alang.empty() && p.spec.slang == "pt-BR", "open: a hostile language code is dropped (S1), pt-BR kept");
    }
    ExpectEq(Eval("med", "local a, b = wum.media.open{url = 'https://youtu.be/dQw4w9WgXcQ'} return tostring(a) .. ':' .. b"), "nil:busy",
             "one player per mod");
    ExpectEq(Eval("med", "return tostring(getmetatable(P))"), "locked", "the player's metatable is locked");
    Expect(starts(Eval("med", "return pcall(setmetatable, P, {})"), "false"), "setmetatable on a player fails");
    Expect(starts(Eval("med", "return pcall(function() P.x = 1 end)"), "false"), "a player has no fields to set");

    // ---- player methods
    ExpectEq(Eval("med", "return tostring(P:play()) .. tostring(P:seek(30, 'keyframes')) .. tostring(P:speed(1.5)) .. "
                         "tostring(P:volume(55)) .. tostring(P:mute(true)) .. tostring(P:track('sub', false)) .. "
                         "tostring(P:track('audio', 2)) .. tostring(P:quality(480)) .. tostring(P:size(640, 360))"),
             "truetruetruetruetruetruetruetruetrue", "every player method answers true");
    if (!fm::g_players.empty()) {
        const fm::Player& p = fm::g_players.begin()->second;
        Expect(!p.paused && p.lastSeek == 30 && p.lastSeekMode == "keyframes" && p.speed == 1.5 && p.volume == 55 && p.mute && p.sid == 0 &&
                   p.aid == 2 && p.quality == 480 && p.w == 640 && p.h == 360,
               "the methods reach the host");
    }
    ExpectEq(Eval("med", "P:seek(5) return 'ok'"), "ok", "seek with the default mode");
    if (!fm::g_players.empty()) ExpectEq(fm::g_players.begin()->second.lastSeekMode, "exact", "the default seek mode is exact");
    Expect(starts(Eval("med", "return pcall(P.seek, P, 'x')"), "false"), "seek: a non-number raises");
    Expect(starts(Eval("med", "return pcall(P.seek, P, 1, 'warp')"), "false"), "seek: an unknown mode raises");
    Expect(starts(Eval("med", "return pcall(P.track, P, 'video', 1)"), "false"), "track: an unknown kind raises");
    Expect(starts(Eval("med", "return pcall(P.mute, P, 1)"), "false"), "mute: a non-boolean raises");
    Expect(starts(Eval("med", "return pcall(P.play, {})"), "false"), "a method on a non-player raises");
    ExpectEq(Eval("med", "local a, b = P:track('sub', 0) return tostring(a) .. ':' .. tostring(b)"), "nil:invalid", "track id 0 is not a track");
    ExpectEq(Eval("med", "local s = P:state() return s.time .. ' ' .. s.duration .. ' ' .. s.tracks.sub[2].lang .. ' ' .. s.helper .. ' ' .. "
                         "tostring(s.drift) .. ' ' .. s.syncAction .. ' ' .. tostring(s.following) .. ' ' .. s.w .. 'x' .. s.h"),
             "12.5 120.0 de ready nil none false 1920x1080", "state(): a table, no drift while not following");

    // ---- follow
    ExpectEq(Eval("med", "return tostring(P:follow({e = 3, i = 7, s = 'play', p = 10, r = 1, a = 5000}, '76561198000000001'))"), "true",
             "follow a host");
    if (!fm::g_players.empty()) {
        const fm::Player& p = fm::g_players.begin()->second;
        Expect(p.following && p.hostId == 76561198000000001ull && p.follow.e == 3 && p.follow.i == "7" && p.follow.play && p.follow.p == 10 &&
                   p.follow.a == 5000,
               "follow: the state and the host's SteamID reach the host");
    }
    ExpectEq(Eval("med", "local s = P:state() return s.drift .. ' ' .. s.syncAction"), "0.05 nudge", "state(): drift and syncAction");
    ExpectEq(Eval("med", "local a, b = P:follow({e = 1, s = 'stop', p = 1, r = 1, a = 1}, '76561198000000001') return tostring(a) .. ':' .. b"),
             "nil:invalid", "follow: s must be play or pause");
    ExpectEq(Eval("med", "local a, b = P:follow({e = 1, s = 'play', p = 1, r = 1, a = 1}, 'host') return tostring(a) .. ':' .. b"),
             "nil:invalid", "follow: the host id must be a SteamID");
    ExpectEq(Eval("med", "local a, b = P:follow({e = 1, s = 'play', p = 1, a = 1}, '76561198000000001') return tostring(a) .. ':' .. b"),
             "nil:invalid", "follow: a missing rate");
    Expect(starts(Eval("med", "return pcall(P.follow, P, 5, '1')"), "false"), "follow: a non-table raises");
    ExpectEq(Eval("med", "return tostring(P:follow(nil))"), "true", "follow(nil) stops");
    if (!fm::g_players.empty()) Expect(!fm::g_players.begin()->second.following, "following stopped");

    // ---- owner-only events
    fm::Event("med", R"({"type":"loaded","title":"Song","duration":3})");
    fm::Event("med2", R"({"type":"helper","state":"ready"})");
    Frames(1);
    ExpectEq(Eval("med", "return table.concat(evs, ',')"), "loaded:Song", "a media event reaches its mod");
    ExpectEq(Eval("med2", "return table.concat(evs, ',')"), "helper", "and only that mod");

    // ---- handles are per generation
    ExpectEq(Eval("med2", "local a = wum.media.open{url = 'https://youtu.be/dQw4w9WgXcQ'} return type(a)"), "userdata",
             "another mod opens its own player");
    Expect(fm::g_players.size() == 2, "two mods, two players");
    ExpectEq(Eval("med", "return tostring(P:close())"), "true", "close");
    ExpectEq(Eval("med", "local a, b = P:play() return tostring(a) .. ':' .. b"), "nil:closed", "a closed player answers nil, closed");
    ExpectEq(Eval("med", "local a, b = P:state() return tostring(a) .. ':' .. b"), "nil:closed", "state of a closed player");
    ExpectEq(Eval("med", "return tostring(P:close())"), "nil", "close twice");
    Expect(fm::g_players.size() == 1, "the host closed it");
    ExpectEq(Eval("med", "P = wum.media.open{url = 'https://youtu.be/dQw4w9WgXcQ'} return type(P)"), "userdata", "open again after close");
    sandbox::ReloadMod("med");
    size_t medPlayers = 0;
    for (const auto& [id, p] : fm::g_players) medPlayers += p.mod == "med";
    Expect(medPlayers == 0 && fm::g_players.size() == 1, "a reload closes the mod's player (Gen cleanup), not the other mod's");

    // ---- resolve
    ExpectEq(Eval("med", "RID = wum.media.resolve('https://www.youtube.com/playlist?list=PL0123456789ab', function(r) GOT = r end) "
                         "return math.type(RID)"),
             "integer", "resolve returns a request id");
    uint32_t rid = 0;
    for (const auto& [id, r] : fm::g_resolves)
        if (r.mod == "med") rid = id;
    Expect(rid != 0 && fm::g_resolves[rid].url == "https://www.youtube.com/playlist?list=PL0123456789ab", "resolve: the canonical URL");
    fm::AnswerResolve(rid, R"({"title":"L","items":[{"url":"https://www.youtube.com/watch?v=aaaaaaaaaaa","id":"aaaaaaaaaaa","title":"A","duration":61}]})");
    ExpectEq(Eval("med", "return GOT.title .. ' ' .. #GOT.items .. ' ' .. GOT.items[1].title .. ' ' .. GOT.items[1].duration"), "L 1 A 61",
             "resolve: the callback gets the items");
    ExpectEq(Eval("med", "local a, b = wum.media.resolve('magnet:?xt=urn:btih:0000000000000000000000000000000000000000', print) "
                         "return tostring(a) .. ':' .. b"),
             "nil:unsupported-url", "resolve: YouTube only");
    Expect(starts(Eval("med", "return pcall(wum.media.resolve, 'https://youtu.be/dQw4w9WgXcQ')"), "false"), "resolve: fn is required");
    ExpectEq(Eval("med", "local n = 0 for i = 1, 4 do if wum.media.resolve('https://youtu.be/dQw4w9WgXcQ', print) then n = n + 1 end end "
                         "local a, b = wum.media.resolve('https://youtu.be/dQw4w9WgXcQ', print) return n .. ' ' .. b"),
             "4 busy", "resolve: at most 4 in flight");
    sandbox::ReloadMod("med");
    size_t medResolves = 0;
    for (const auto& [id, r] : fm::g_resolves) medResolves += r.mod == "med";
    Expect(medResolves == 0, "a reload cancels the mod's resolves");

    // ---- torrents
    ExpectEq(Eval("med", "local a, b = wum.media.torrent('https://youtu.be/dQw4w9WgXcQ') return tostring(a) .. ':' .. b"),
             "nil:unsupported-url", "torrent: a magnet only");
    sandbox::Game().inMatch = true;
    ExpectEq(Eval("med", "local a, b = wum.media.torrent('magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567') "
                         "return tostring(a) .. ':' .. b"),
             "nil:first-use-in-match", "torrent: the match flag reaches the host");
    sandbox::Game().inMatch = false;
    ExpectEq(Eval("med", "T = wum.media.torrent('magnet:?xt=urn:btih:0123456789ABCDEF0123456789ABCDEF01234567&x.pe=1.2.3.4:5', "
                         "function(r) TF = r end) return tostring(T) .. ' ' .. tostring(T:files())"),
             "media torrent nil", "torrent: a handle, no files yet");
    uint32_t tid = 0;
    for (const auto& [id, t] : fm::g_torrents)
        if (t.mod == "med") tid = id;
    Expect(tid && fm::g_torrents[tid].magnet == "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567",
           "torrent: the host gets the canonical magnet (x.pe dropped)");
    fm::TorrentFilesArrive(tid);
    ExpectEq(Eval("med", "local f = T:files() return TF.name .. ' ' .. #TF.files .. ' ' .. #f .. ' ' .. tostring(f[1].video) .. ' ' .. "
                         "tostring(f[2].video) .. ' ' .. T:status().peers"),
             "Movie 2 2 true false 3", "torrent: the callback and files()");
    ExpectEq(Eval("med", "local a, b = wum.media.open{torrent = T, file = 1} return tostring(a) .. ':' .. b"), "nil:unsupported-url",
             "open: a non-video torrent file");
    ExpectEq(Eval("med", "local a, b = wum.media.open{torrent = T, file = 0.5} return tostring(a) .. ':' .. b"), "nil:unsupported-url",
             "open: a fractional file index");
    ExpectEq(Eval("med", "P = wum.media.open{torrent = T, file = 0} return type(P)"), "userdata", "open: a torrent file");
    ExpectEq(Eval("med", "return tostring(T:close()) .. ' ' .. tostring(T:files())"), "true nil", "t:close, then files() is nil");
    ExpectEq(Eval("med", "local a, b = T:status() return tostring(a) .. ':' .. b"), "nil:closed", "status of a closed torrent");
    ExpectEq(Eval("med", "local n = 0 for i = 1, 5 do if wum.media.torrent('magnet:?xt=urn:btih:000000000000000000000000000000000000000' .. i) "
                         "then n = n + 1 end end return n"),
             "4", "at most 4 torrents per mod");
    sandbox::UnloadMod("med");
    sandbox::UnloadMod("med2");
    size_t left = 0;
    for (const auto& [id, t] : fm::g_torrents) left += t.mod == "med";
    Expect(left == 0 && fm::g_players.empty(), "unloading closes every player and torrent");
}
