// sandbox_selftest cases for wum.audio.music (owner: C1). Included in the test file's anonymous namespace after the
// helpers; defines TestMusic(), run before the docs check.
// Melange 0.12 wum.audio.music (C1): per-mod requests ORed, release on unload, the options, and kind="music".
void TestMusicApi() {
    fakemusic::g_musicAllow = true;
    fakemusic::g_musicDead = false;
    fakemusic::g_musicLive = true;
    fakemusic::g_musicReq.clear();
    fake::g_audioVoices.clear();
    fake::g_audioReleases = 0;
    const std::string lua = R"lua(
function ask(on, o) local a, b = wum.audio.music.suppress(on, o); return tostring(a) .. ':' .. tostring(b) end
function state() local e, m = wum.audio.music.suppressed(); return tostring(e) .. ',' .. tostring(m) end
h = wum.audio.load("m.wav")
)lua";
    SetMod("musa", lua);
    SetMod("musb", lua);
    for (const char* id : {"musa", "musb"}) WriteText(fake::g_modsDir + L"\\" + W(id) + L"\\m.wav", TestWavBytes(22050, 100));
    Expect(sandbox::LoadMod("musa") && sandbox::LoadMod("musb"), "music mods load");

    ExpectEq(Eval("musa", "return tostring(wum.audio.music.available())"), "true", "available follows the backend");
    ExpectEq(Eval("musa", "return state()"), "false,false", "nothing asked at first");
    ExpectEq(Eval("musa", "return ask(true)"), "true:nil", "suppress(true) is accepted");
    Expect(fakemusic::g_musicFade < 0, "no fade option means the default");
    ExpectEq(Eval("musa", "return state()"), "true,true", "the asking mod sees effective and mine");
    ExpectEq(Eval("musb", "return state()"), "true,false", "another mod sees it effective, not its own (OR across mods)");
    ExpectEq(Eval("musb", "return ask(true, {fade = 0})"), "true:nil", "a second mod can ask too");
    Expect(fakemusic::g_musicFade == 0.0f, "fade = 0 reaches the audio module");
    ExpectEq(Eval("musa", "return ask(false, {fade = 99})"), "true:nil", "release is accepted");
    Expect(fakemusic::g_musicFade == 10.0f, "the fade is clamped to 10 s");
    ExpectEq(Eval("musa", "return state()"), "true,false", "still effective while another mod asks");
    ExpectEq(Eval("musb", "return ask(false)"), "true:nil", "the last release");
    ExpectEq(Eval("musb", "return state()"), "false,false", "nothing asked any more");

    Expect(Eval("musa", "return ask(1)").rfind("ERR:", 0) == 0, "suppress needs a boolean");
    Expect(Eval("musa", "return ask(true, 5)").rfind("ERR:", 0) == 0, "opts must be a table");
    Expect(Eval("musa", "return ask(true, {fade = 'x'})").rfind("ERR:", 0) == 0, "fade must be a number");

    fakemusic::g_musicAllow = false;
    ExpectEq(Eval("musa", "return ask(true)"), "nil:disabled", "[Audio] AllowMusicSuppress=0");
    fakemusic::g_musicAllow = true;
    fakemusic::g_musicDead = true;
    ExpectEq(Eval("musa", "return ask(true)"), "nil:unavailable", "FMOD cannot be driven");
    fakemusic::g_musicDead = false;
    fakemusic::g_musicLive = false;
    ExpectEq(Eval("musa", "return tostring(wum.audio.music.available())"), "false", "available is false until the category is found");
    ExpectEq(Eval("musa", "return ask(true)"), "true:nil", "a request before FMOD is up is kept");
    fakemusic::g_musicLive = true;

    // kind = "music" routes a voice to the music submix; sfx is the default.
    ExpectEq(Eval("musa", "return math.type(wum.audio.play(h, {kind = 'music', loop = true}))"), "integer", "kind = music plays");
    ExpectEq(Eval("musa", "return math.type(wum.audio.play(h, {kind = 'sfx'}))"), "integer", "kind = sfx plays");
    ExpectEq(Eval("musa", "return math.type(wum.audio.play(h))"), "integer", "no kind plays");
    int music = 0, sfx = 0;
    for (const auto& [id, v] : fake::g_audioVoices) (v.opts.music ? music : sfx)++;
    Expect(music == 1 && sfx == 2, "only kind = music is marked as music");
    Expect(Eval("musa", "return wum.audio.play(h, {kind = 'loud'})").rfind("ERR:", 0) == 0, "an unknown kind is an error");
    Expect(Eval("musa", "return wum.audio.play(h, {kind = 3})").rfind("ERR:", 0) == 0, "kind must be a string");

    // Unload (and reload, and disable) release the request through the generation's cleanup.
    ExpectEq(Eval("musb", "return state()"), "true,false", "musa still asks");
    sandbox::UnloadMod("musa");
    ExpectEq(Eval("musb", "return state()"), "false,false", "unloading the mod releases its request");
    ExpectEq(Eval("musb", "return ask(true)"), "true:nil", "musb asks");
    Expect(sandbox::ReloadMod("musb"), "musb reloads");
    ExpectEq(Eval("musb", "return state()"), "false,false", "a reload releases the request too");
    sandbox::UnloadMod("musb");

    // The console has no mod: it can ask and release as well.
    ExpectEq(Eval(nullptr, "local a = wum.audio.music.suppress(true); return tostring(a) .. ',' .. tostring((wum.audio.music.suppressed()))"),
             "true,true", "the console can suppress");
    ExpectEq(Eval(nullptr, "wum.audio.music.suppress(false); return tostring((wum.audio.music.suppressed()))"), "false",
             "and release");
}

// Melange 0.17: wum.audio.voice (lines, load), cut, unload and set, through the fakes in sandbox_fakes_audio.inc.
void TestVoiceApi() {
    fake::g_audioAvail = true;
    fake::g_audioVoices.clear();
    fake::g_audioClips.clear();
    SetMod("voa", R"lua(
function r(...) local t = table.pack(...); for i = 1, t.n do t[i] = tostring(t[i]) end; return table.concat(t, ",") end
)lua");
    SetMod("vob", R"lua(
function r(...) local t = table.pack(...); for i = 1, t.n do t[i] = tostring(t[i]) end; return table.concat(t, ",") end
)lua");
    Expect(sandbox::LoadMod("voa") && sandbox::LoadMod("vob"), "voice mods load");
    ExpectEq(Eval(nullptr, "return type(wum.audio.voice.lines)"), "function", "wum.audio.voice exists");
    Expect(Eval(nullptr, "wum.audio.voice.load = nil").rfind("ERR:", 0) == 0, "wum.audio.voice is read-only");

    ExpectEq(Eval("voa", "local t = wum.audio.voice.lines('VOFAKE'); return r(#t, t[1].name, t[1].seconds, t[2].name, t[2].seconds)"),
             "2,Nooo_01,0.5,taunt 2,0.25", "lines: names verbatim and seconds, bank name in any case");
    ExpectEq(Eval("voa", "return r(wum.audio.voice.lines('vonone'))"), "nil,unknown bank", "lines: unknown bank");
    ExpectEq(Eval("voa", "return r(wum.audio.voice.lines('vobroken'))"), "nil,unreadable bank", "lines: unreadable bank");
    Expect(Eval("voa", "return wum.audio.voice.lines(nil)").rfind("ERR:", 0) == 0, "lines: the bank must be a string");

    ExpectEq(Eval("voa", "h = wum.audio.voice.load('vofake', 'nooo_01'); return math.type(h)"), "integer", "load: a handle");
    ExpectEq(Eval("voa", "return tostring(wum.audio.voice.load('vofake', 'Nooo_01') == h)"), "true", "load: the same line, the same handle");
    ExpectEq(Eval("voa", "return r(wum.audio.voice.load('vofake', 'nope'))"), "nil,unknown line", "load: unknown line");
    ExpectEq(Eval("voa", "return r(wum.audio.voice.load('vox', 'nope'))"), "nil,unknown bank", "load: unknown bank");
    Expect(Eval("voa", "return wum.audio.voice.load('vofake')").rfind("ERR:", 0) == 0, "load: the line must be a string");

    // cut: a new handle, the same for the same arguments, errors as nil, reason.
    ExpectEq(Eval("voa", "c = wum.audio.cut(h, 0.1, 0.2); return r(math.type(c), c ~= h, wum.audio.cut(h, 0.1, 0.2) == c)"),
             "integer,true,true", "cut: a new handle, stable for the same arguments");
    ExpectEq(Eval("voa", "return r(wum.audio.cut(h, 0.6, 1))"), "nil,empty cut", "cut: past the end");
    ExpectEq(Eval("voa", "return r(wum.audio.cut(9999, 0, 1))"), "nil,unknown sound", "cut: unknown handle");
    ExpectEq(Eval("voa", "return r(math.type(wum.audio.cut(h, 0, 0.1, 0.5)))"), "integer", "cut: an explicit fade");
    Expect(Eval("voa", "return wum.audio.cut(h, 'a', 1)").rfind("ERR:", 0) == 0, "cut: from must be a number");
    Expect(Eval("voa", "return wum.audio.cut(h, 0, 1, 'x')").rfind("ERR:", 0) == 0, "cut: fade must be a number");
    const std::string h = Eval("voa", "return h");
    ExpectEq(Eval("vob", "return r(wum.audio.cut(" + h + ", 0, 0.1))"), "nil,unknown sound", "cut: another mod's handle is unknown");

    // set: a playing voice of this mod; volume and pitch clamped as for play.
    ExpectEq(Eval("voa", "v = wum.audio.play(c); return r(wum.audio.set(v, {volume = 5, pitch = 0.1}))"), "true", "set: own voice");
    Expect(!fake::g_audioVoices.empty(), "a voice plays");
    if (!fake::g_audioVoices.empty()) {
        const auto& pv = fake::g_audioVoices.rbegin()->second;
        Expect(pv.opts.volume == 2.0f && pv.opts.pitch == 0.5f, "set: clamped to 2 and 0.5");
        ExpectEq(Eval("voa", "return r(wum.audio.set(v, {pitch = 0/0}))"), "true", "set: a NaN pitch is ignored");
        Expect(pv.opts.pitch == 0.5f, "set: NaN keeps the pitch");
        ExpectEq(Eval("voa", "return r(wum.audio.set(v, {volume = 0.3}))"), "true", "set: volume only");
        Expect(std::fabs(pv.opts.volume - 0.3f) < 1e-6f && pv.opts.pitch == 0.5f, "set: volume applied, pitch kept");
    }
    const std::string v = Eval("voa", "return v");
    ExpectEq(Eval("vob", "return r(wum.audio.set(" + v + ", {volume = 1}))"), "false", "set: another mod's voice");
    ExpectEq(Eval("voa", "return r(wum.audio.set(9999, {}))"), "false", "set: unknown voice");
    Expect(Eval("voa", "return wum.audio.set(v)").rfind("ERR:", 0) == 0, "set: opts must be a table");
    Expect(Eval("voa", "return wum.audio.set(v, {volume = 'x'})").rfind("ERR:", 0) == 0, "set: volume must be a number");

    // unload: frees one clip; the voice playing it is left alone.
    ExpectEq(Eval("voa", "return r(wum.audio.unload(c), wum.audio.unload(c), wum.audio.unload(-1))"), "true,false,false",
             "unload: once, then unknown");
    ExpectEq(Eval("voa", "return r(wum.audio.play(c))"), "nil,unknown sound", "an unloaded handle does not play");
    ExpectEq(Eval("voa", "return r(wum.audio.set(v, {volume = 1}))"), "true", "a voice of an unloaded clip plays on");
    ExpectEq(Eval("vob", "return r(wum.audio.unload(" + h + "))"), "false", "unload: another mod's handle");
    Expect(Eval("voa", "return wum.audio.unload('x')").rfind("ERR:", 0) == 0, "unload: the handle must be a number");
    sandbox::UnloadMod("voa");
    sandbox::UnloadMod("vob");
}

void TestMusic() {
    ExpectEq(Eval(nullptr, "return type(wum.audio.music.suppress)"), "function", "wum.audio.music exists");
    Expect(Eval(nullptr, "wum.audio.music.suppress = nil").rfind("ERR:", 0) == 0, "wum.audio.music is read-only");
    TestMusicApi();
    TestVoiceApi();
}

