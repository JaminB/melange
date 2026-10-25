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

void TestMusic() {
    ExpectEq(Eval(nullptr, "return type(wum.audio.music.suppress)"), "function", "wum.audio.music exists");
    Expect(Eval(nullptr, "wum.audio.music.suppress = nil").rfind("ERR:", 0) == 0, "wum.audio.music is read-only");
    TestMusicApi();
}

