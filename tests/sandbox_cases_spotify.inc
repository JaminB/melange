// sandbox_selftest cases for wum.spotify (Melange 0.13). Included in the test file's anonymous namespace after the
// helpers (SetMod, Eval, Frames, Expect, ExpectEq); defines TestSpotify(), run before the docs check. The Spotify host
// is a fake (sandbox_fakes_spotify.inc), so these cases cover the binding: argument types and values, the connect
// click, callbacks, owner-only events and the cleanup on unload.
void TestSpotify() {
    namespace fs = fake_spotify;
    using spotify::logic::Op;
    auto starts = [](const std::string& s, const char* p) { return s.rfind(p, 0) == 0; };
    auto has = [](const std::string& s, const char* p) { return s.find(p) != std::string::npos; };
    auto last = []() -> const fs::Submitted* { return fs::g_submitted.empty() ? nullptr : &fs::g_submitted.back(); };

    // ---- the namespace
    ExpectEq(Eval(nullptr, "return type(wum.spotify)"), "table", "wum.spotify exists");
    Expect(starts(Eval(nullptr, "return pcall(function() wum.spotify.play = print end)"), "false"), "wum.spotify is frozen");
    Expect(starts(Eval(nullptr, "return pcall(function() wum.spotify.x = 1 end)"), "false"), "no new fields in wum.spotify");

    // ---- available() and status() (they work in the console too)
    ExpectEq(Eval(nullptr, "return tostring(wum.spotify.available())"), "true", "available() with the host up");
    for (const char* reason : {"disabled", "unavailable", "no-client-id"}) {
        fs::g_available = false;
        fs::g_reason = reason;
        ExpectEq(Eval(nullptr, "local a, b = wum.spotify.available() return tostring(a) .. ':' .. tostring(b)"), std::string("false:") + reason,
                 std::string("available() passes the reason ") + reason);
    }
    fs::g_available = true;
    ExpectEq(Eval(nullptr, "local s = wum.spotify.status() return s.state .. ' ' .. tostring(s.premium) .. ' ' .. tostring(s.linked) .. ' ' .. "
                           "tostring(s.user) .. ' ' .. tostring(s.error)"),
             "disconnected false false nil nil", "status(): the console sees the global state");
    for (const char* fn : {"connect()", "disconnect()", "library({kind = 'tracks'}, print)", "devices(print)", "play({uri = 'x'})",
                           "pause()", "resume()", "seek(1)", "volume(1)", "playback(print)"})
        Expect(has(Eval(nullptr, std::string("return pcall(function() return wum.spotify.") + fn + " end)"), "needs a mod context"),
               std::string("console: ") + fn + " needs a mod context");

    SetMod("spo", "evs = {} wum.events.on('melange.spotify', function(e) evs[#evs + 1] = e.type .. ':' .. e.state end)");
    SetMod("spo2", "evs = {} wum.events.on('melange.spotify', function(e) evs[#evs + 1] = e.type .. ':' .. e.state end)");
    Expect(sandbox::LoadMod("spo") && sandbox::LoadMod("spo2"), "spotify mods load");

    fs::g_status = {"connected", "Ann", true, true, ""};
    ExpectEq(Eval("spo", "local s = wum.spotify.status() return s.state .. ' ' .. tostring(s.linked) .. ' ' .. tostring(s.user)"),
             "disconnected true nil", "status(): linked but not granted to this mod");
    Expect(fs::g_touched.count("spo") == 1, "status() touches the mod (it gets events)");
    fs::g_granted.insert("spo2");
    ExpectEq(Eval("spo2", "local s = wum.spotify.status() return s.state .. ' ' .. s.user .. ' ' .. tostring(s.premium) .. ' ' .. tostring(s.linked)"),
             "connected Ann true true", "status(): the granted mod sees the account");
    fs::g_granted.clear();
    fs::g_status = {"error", "", false, false, "denied"};
    ExpectEq(Eval("spo", "local s = wum.spotify.status() return s.state .. ' ' .. s.error"), "error denied", "status(): the error code");
    fs::g_status = {"disconnected", "", false, false, ""};

    // ---- connect(): a click on the mod's own region
    ExpectEq(Eval("spo", "local a, b = wum.spotify.connect() return tostring(a) .. ':' .. tostring(b)"), "nil:gesture", "connect without a click");
    input::gesture::Mark("spo", false);
    ExpectEq(Eval("spo", "local a, b = wum.spotify.connect() return tostring(a) .. ':' .. tostring(b)"), "nil:gesture",
             "connect after a click that is not consent-grade");
    input::gesture::Mark("spo2", true);
    ExpectEq(Eval("spo", "local a, b = wum.spotify.connect() return tostring(a) .. ':' .. tostring(b)"), "nil:gesture",
             "another mod's click does not count");
    input::gesture::Mark("spo", true);
    ExpectEq(Eval("spo", "return tostring(wum.spotify.connect())"), "true", "connect right after a consent-grade click");
    Expect(fs::g_connects.size() == 1 && fs::g_connects[0] == "spo", "host::Connect called once, for the clicking mod");
    Expect(!input::gesture::Recent("spo", 60000, false), "connect consumed the click");
    ExpectEq(Eval("spo", "local a, b = wum.spotify.connect() return tostring(a) .. ':' .. tostring(b)"), "nil:gesture", "one click, one connect");
    fs::g_granted.insert("spo");
    ExpectEq(Eval("spo", "return tostring(wum.spotify.connect())"), "true", "connect when already granted needs no click");
    Expect(fs::g_connects.size() == 1, "and does not call the host");
    fs::g_granted.clear();
    fs::g_connectOk = false;
    for (const char* reason : {"busy", "port-busy", "browser", "no-client-id"}) {
        fs::g_connectReason = reason;
        input::gesture::Mark("spo", true);
        ExpectEq(Eval("spo", "local a, b = wum.spotify.connect() return tostring(a) .. ':' .. tostring(b)"), std::string("nil:") + reason,
                 std::string("connect passes the host's reason ") + reason);
    }
    Expect(input::gesture::Recent("spo", 2000), "a failed connect keeps the click");
    fs::g_connectOk = true;
    input::gesture::Clear("spo");
    fs::g_available = false;
    fs::g_reason = "no-client-id";
    ExpectEq(Eval("spo", "local a, b = wum.spotify.connect() return tostring(a) .. ':' .. tostring(b)"), "nil:no-client-id",
             "connect when unavailable");
    fs::g_available = true;

    // ---- requests before the mod is granted
    ExpectEq(Eval("spo", "local a, b = wum.spotify.devices(print) return tostring(a) .. ':' .. tostring(b)"), "nil:auth",
             "a request without a grant is auth");
    fs::g_granted.insert("spo");

    // ---- argument types raise, bad values are "invalid"
    Expect(starts(Eval("spo", "return pcall(wum.spotify.play, 'spotify:track:4uLU6hMCjMI75M1A2tKUQC')"), "false"), "play: opts must be a table");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.play, {uri = 5})"), "false"), "play: a non-string uri raises");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.play, {uris = 'x'})"), "false"), "play: uris must be a table");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.play, {uris = {1}})"), "false"), "play: uris must hold strings");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.play, {uri = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC'}, 5)"), "false"),
           "play: fn must be a function");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.library, 'tracks', print)"), "false"), "library: opts must be a table");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.library, {kind = 'tracks'})"), "false"), "library: fn is required");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.library, {}, print)"), "false"), "library: kind is required");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.library, {kind = 'tracks', offset = '1'}, print)"), "false"),
           "library: a string offset raises");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.seek, 'soon')"), "false"), "seek: a non-number raises");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.seek, '1000')"), "false"), "seek: a numeric string raises too");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.volume, '50')"), "false"), "volume: a numeric string raises");
    Expect(starts(Eval("spo", "return pcall(wum.spotify.devices)"), "false"), "devices: fn is required");
    const size_t before = fs::g_submitted.size();
    for (const char* bad : {"wum.spotify.play{uri = 'spotify:album:4uLU6hMCjMI75M1A2tKUQC'}", "wum.spotify.play{uris = {}}",
                            "wum.spotify.play{uri = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC', context = 'spotify:playlist:37i9dQZF1DXcBWIGoYBM5M'}",
                            "wum.spotify.play{}",
                            "wum.spotify.play{uri = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC', positionMs = -1}",
                            "wum.spotify.play{uri = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC', positionMs = 0/0}",
                            "wum.spotify.play{uri = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC', positionMs = 1.5}",
                            "wum.spotify.play{uri = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC', deviceId = 'a b'}",
                            "wum.spotify.play{uri = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC', offset = 1}",
                            "wum.spotify.play{context = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC'}",
                            "wum.spotify.play{context = 'spotify:playlist:37i9dQZF1DXcBWIGoYBM5M', offset = 'spotify:album:4uLU6hMCjMI75M1A2tKUQC'}",
                            "wum.spotify.library({kind = 'nope'}, print)", "wum.spotify.library({kind = 'playlist'}, print)",
                            "wum.spotify.library({kind = 'playlist', id = '../me'}, print)", "wum.spotify.library({kind = 'tracks', limit = 51}, print)",
                            "wum.spotify.library({kind = 'tracks', offset = -1}, print)", "wum.spotify.seek(-5)", "wum.spotify.seek(1/0)",
                            "wum.spotify.volume(101)", "wum.spotify.volume(50.5)"}) {
        const std::string r = Eval("spo", std::string("local a, b = ") + bad + " return tostring(a) .. ':' .. tostring(b)");
        ExpectEq(r, "nil:invalid", std::string("invalid: ") + bad);
    }
    Expect(fs::g_submitted.size() == before, "nothing invalid reached the host");
    std::string uris51 = "local t = {} for i = 1, 51 do t[i] = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC' end ";
    ExpectEq(Eval("spo", uris51 + "local a, b = wum.spotify.play{uris = t} return tostring(a) .. ':' .. tostring(b)"), "nil:invalid", "play: 51 uris");

    // ---- good requests reach the host
    ExpectEq(Eval("spo", "RID = wum.spotify.play({uri = 'spotify:track:4uLU6hMCjMI75M1A2tKUQC', positionMs = 1500, deviceId = 'dev_1'}, "
                         "function(r) GOT = r; N = (N or 0) + 1 end) return math.type(RID)"),
             "integer", "play returns an integer request id");
    const fs::Submitted* s = last();
    Expect(s && s->mod == "spo" && s->req.op == Op::Play && s->req.uris.size() == 1 && s->req.uris[0] == "spotify:track:4uLU6hMCjMI75M1A2tKUQC" &&
               s->req.positionMs == 1500 && s->req.deviceId == "dev_1" && s->req.context.empty() && s->token != 0,
           "play: the request's fields");
    const uint32_t playRid = s ? s->rid : 0;
    ExpectEq(Eval("spo", "return tostring(RID)"), std::to_string(playRid), "the rid is the host's");
    ExpectEq(Eval("spo", "return math.type(wum.spotify.play{context = 'spotify:playlist:37i9dQZF1DXcBWIGoYBM5M', offset = 3})"), "integer",
             "play a playlist from an index, without fn");
    s = last();
    Expect(s && s->req.context == "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M" && s->req.offsetIndex == 3 && s->req.offsetUri.empty() &&
               s->req.positionMs == -1 && s->token == 0,
           "play: context and offset index, no callback");
    Eval("spo", "wum.spotify.play{context = 'spotify:playlist:37i9dQZF1DXcBWIGoYBM5M', offset = 'spotify:track:1301WleyT98MSxVHPZCA6M'}");
    s = last();
    Expect(s && s->req.offsetIndex == -1 && s->req.offsetUri == "spotify:track:1301WleyT98MSxVHPZCA6M", "play: offset by uri");
    Eval("spo", "wum.spotify.play{uris = {'spotify:track:4uLU6hMCjMI75M1A2tKUQC', 'spotify:track:1301WleyT98MSxVHPZCA6M'}}");
    s = last();
    Expect(s && s->req.uris.size() == 2, "play: uris in order");
    Eval("spo", "wum.spotify.library({kind = 'playlist', id = '37i9dQZF1DXcBWIGoYBM5M', offset = 50, limit = 10}, print)");
    s = last();
    Expect(s && s->req.op == Op::PlaylistTracks && s->req.id == "37i9dQZF1DXcBWIGoYBM5M" && s->req.offset == 50 && s->req.limit == 10,
           "library: a playlist's tracks");
    Eval("spo", "wum.spotify.library({kind = 'playlists'}, print)");
    s = last();
    Expect(s && s->req.op == Op::Playlists && s->req.offset == 0 && s->req.limit == 50, "library: playlists with the defaults");
    Eval("spo", "wum.spotify.seek(61000) wum.spotify.volume(35) wum.spotify.pause() wum.spotify.resume(print)");
    const size_t n = fs::g_submitted.size();
    Expect(n >= 4 && fs::g_submitted[n - 4].req.op == Op::Seek && fs::g_submitted[n - 4].req.positionMs == 61000 &&
               fs::g_submitted[n - 3].req.op == Op::Volume && fs::g_submitted[n - 3].req.volume == 35 &&
               fs::g_submitted[n - 2].req.op == Op::Pause && fs::g_submitted[n - 1].req.op == Op::Resume && fs::g_submitted[n - 1].token != 0,
           "seek, volume, pause, resume");

    // ---- answers: once
    fs::AnswerRequest(playRid, R"({"ok":true})");
    ExpectEq(Eval("spo", "return tostring(GOT.ok) .. ' ' .. N"), "true 1", "the answer reaches fn as a table");
    fs::AnswerRequest(playRid, R"({"error":"rate","message":"x","retryAfter":3})");
    ExpectEq(Eval("spo", "return tostring(GOT.ok) .. ' ' .. N"), "true 1", "a second answer is ignored (done killed the callback)");
    ExpectEq(Eval("spo", "RID2 = wum.spotify.playback(function(r) PB = r end) return math.type(RID2)"), "integer", "playback");
    s = last();
    const uint32_t pbRid = s ? s->rid : 0;
    fs::AnswerRequest(pbRid, R"({"error":"no-device","message":"No active device"})");
    ExpectEq(Eval("spo", "return PB.error .. ' ' .. PB.message"), "no-device No active device", "a failure reaches fn as {error, message}");

    // ---- owner-only events
    fs::EmitStatus("spo", R"({"type":"status","state":"connected","user":"Ann","premium":true,"linked":true})");
    Frames(1);
    ExpectEq(Eval("spo", "return table.concat(evs, ',')"), "status:connected", "a melange.spotify event reaches its mod");
    ExpectEq(Eval("spo2", "return table.concat(evs, ',')"), "", "and only that mod");

    // ---- disconnect needs no click and leaves nothing to pause
    ExpectEq(Eval("spo2", "return tostring(wum.spotify.disconnect())"), "true", "disconnect");
    Expect(fs::g_disconnects.size() == 1 && fs::g_disconnects[0] == "spo2", "host::Disconnect called");
    fs::g_granted.insert("spo");

    // ---- unload cancels outstanding requests (and never calls the host to pause)
    ExpectEq(Eval("spo", "OUT = wum.spotify.devices(function() end) return math.type(OUT)"), "integer", "an outstanding request");
    s = last();
    const uint32_t outRid = s ? s->rid : 0;
    const size_t submittedBefore = fs::g_submitted.size();
    sandbox::UnloadMod("spo");
    Expect(outRid && fs::Cancelled(outRid), "unloading cancels the mod's outstanding request");
    Expect(fs::g_submitted.size() == submittedBefore, "unloading queues nothing (no pause)");
    fs::AnswerRequest(outRid, R"({"items":[]})");  // a late answer for an unloaded mod is dropped
    sandbox::UnloadMod("spo2");
    fs::g_granted.clear();
}
