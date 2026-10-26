// sandbox_selftest cases for the Melange 0.12 wum.draw functions (owner: C5). Included in the test file's anonymous
// namespace after the helpers; defines TestDrawScreen(), run before the docs check.

// A wum.media player as wum_media_handles.h lays it out (metatable "melange.media.player", {id, serial}), put into the
// mod's environment as global `name` without going through wum.media.
void InjectPlayer(const char* mod, const char* name, uint32_t id, int serialDelta = 0) {
    struct PlayerUd {
        uint32_t id, serial;
    };
    sandbox::ModRec* m = sandbox::FindMod(mod);
    if (!m || !m->gen) return;
    lua_State* L = sandbox::L();
    lua_rawgeti(L, LUA_REGISTRYINDEX, m->gen->envRef);
    auto* ud = static_cast<PlayerUd*>(lua_newuserdatauv(L, sizeof(PlayerUd), 0));
    ud->id = id;
    ud->serial = m->gen->serial + serialDelta;
    luaL_newmetatable(L, "melange.media.player");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

int CountRecords(const std::string& cat, const std::string& msg) {
    int n = 0;
    for (const auto& r : fake::g_recs) n += r.cat == cat && r.msg.rfind(msg, 0) == 0;
    return n;
}

const char kFontJson[] = R"({"format": "holo-font-1", "size": 16, "scale": 2, "lineHeight": 40, "ascent": 30, "outline": 2,
 "glyphs": {"65": [0, 0, 20, 28, 1, -26, 22], "63": [20, 0, 16, 28, 1, -26, 18]}, "kern": {"65,65": -2}})";

void TestDrawScreenStage() {
    for (const char* f : {"hudImageUV", "hudNine", "font", "textSize", "hudClip", "hudClipEnd", "media"})
        ExpectEq(Eval(nullptr, std::string("return type(wum.draw.") + f + ")"), "function", std::string("wum.draw.") + f + " exists");
    SetMod("scr", "frames = 0\nh = wum.draw.on('screen', function(stage) frames = frames + 1; seen = stage end)\n");
    const int before = fake::g_drawCbs;
    Expect(sandbox::LoadMod("scr"), "screen mod loads: " + StatusOf("scr").error);
    Expect(fake::g_drawCbs == before + 1, "wum.draw.on('screen') registers a draw callback");
    ExpectEq(Eval("scr", "return type(h)"), "number", "it returns a handle");
    ExpectEq(Eval("scr", "return wum.draw.off(h)"), "true", "and off removes it");
    Expect(Eval("scr", "wum.draw.on('present', function() end)").find("world, worldLate, hud or screen") != std::string::npos,
           "an unknown stage names the four stages");
    sandbox::UnloadMod("scr");
}

void TestDrawImages() {
    SetMod("img", "tex = wum.draw.texture('a.png') arr = wum.draw.texture('m.png', {layers = 4})\n");
    SetMod("img2", "mine = wum.draw.texture('b.png')\n");
    Expect(sandbox::LoadMod("img") && sandbox::LoadMod("img2"), "image mods load");
    const std::string foreign = Eval("img2", "return mine");

    draw::g_uvCalls.clear();
    Eval("img", "wum.draw.hudImageUV(1, 2, 3, 4, tex, 0.25, 0.5, 0.75, 1, '#ff000080')");
    Expect(draw::g_uvCalls.size() == 1 && draw::g_uvCalls[0].uv[0] == 0.25f && draw::g_uvCalls[0].uv[3] == 1.f &&
               draw::g_uvCalls[0].r[2] == 3.f && draw::g_uvCalls[0].tint == 0x800000ffu,
           "hudImageUV passes the rectangle, uv and tint");
    Expect(Eval("img", "wum.draw.hudImageUV(1, 2, 3, 4, " + foreign + ", 0, 0, 1, 1)").find("this mod's wum.draw.texture") != std::string::npos,
           "hudImageUV refuses another mod's texture");
    Expect(Eval("img", "wum.draw.hudImageUV(1, 2, 3, 4, arr, 0, 0, 1, 1)").find("texture array") != std::string::npos,
           "hudImageUV refuses a texture array");
    Expect(Eval("img", "wum.draw.hudImageUV(1, 2, 3, 4, tex, 0, 0, 1)").rfind("ERR:", 0) == 0, "hudImageUV needs four uv numbers");

    draw::g_nineCalls.clear();
    Eval("img", "wum.draw.hudNine(0, 0, 100, 50, tex, 8, 9, 10, 11)");
    Eval("img", "wum.draw.hudNine(0, 0, 100, 50, tex, 8, 8, 8, 8, {tint = '#00ff00', scale = 0.5, uv = {0, 0.5, 0.5, 1}})");
    Expect(draw::g_nineCalls.size() == 2, "hudNine draws");
    if (draw::g_nineCalls.size() == 2) {
        const auto& a = draw::g_nineCalls[0];
        const auto& b = draw::g_nineCalls[1];
        Expect(a.insets[0] == 8 && a.insets[3] == 11 && a.o.scale == 1 && a.o.uv[2] == 1 && a.o.tint == 0xffffffffu,
               "hudNine defaults: scale 1, the whole texture, white");
        Expect(b.o.scale == 0.5f && b.o.uv[1] == 0.5f && b.o.uv[2] == 0.5f && b.o.tint == 0xff00ff00u, "hudNine opts pass through");
    }
    for (const char* bad : {"{scale = 0}", "{uv = {0, 0, 1}}", "{uv = 'all'}", "{pad = 1}", "'big'"})
        Expect(Eval("img", std::string("wum.draw.hudNine(0, 0, 9, 9, tex, 1, 1, 1, 1, ") + bad + ")").rfind("ERR:", 0) == 0,
               std::string("hudNine raises for opts ") + bad);
    Expect(Eval("img", "wum.draw.hudNine(0, 0, 9, 9, " + foreign + ", 1, 1, 1, 1)").rfind("ERR:", 0) == 0,
           "hudNine refuses another mod's texture");

    // hudImage: another mod's texture (or a made-up id) is dropped with one warning per mod, never raised.
    const int warned = CountRecords("sandbox", "wum.draw.hudImage: texture");
    ExpectEq(Eval("img", "wum.draw.hudImage(0, 0, 1, 1, " + foreign + ") wum.draw.hudImage(0, 0, 1, 1, 99999) return 'ok'"), "ok",
             "hudImage with a foreign texture does not raise");
    Expect(CountRecords("sandbox", "wum.draw.hudImage: texture") == warned + 1, "it warns once for the mod");
    Expect(HasRecord("sandbox", "wum.draw.hudImage: texture", "img"), "the warning names the mod");
    ExpectEq(Eval("img", "wum.draw.hudImage(0, 0, 1, 1, tex) return 'ok'"), "ok", "hudImage with an own texture");
    Expect(CountRecords("sandbox", "wum.draw.hudImage: texture") == warned + 1, "an own texture does not warn");

    // texture{mips = false}: the option loader without mips; {mips = true} alone is the plain HUD texture.
    draw::g_lastOpts = draw::TextureOptions{};
    draw::g_lastOpts.layers = 7;  // sentinel: untouched unless LoadTextureEx runs
    ExpectEq(Eval("img", "return type(wum.draw.texture('hud.png', {mips = true}))"), "number", "{mips = true} loads");
    Expect(draw::g_lastOpts.layers == 7, "{mips = true} alone takes the plain loader");
    ExpectEq(Eval("img", "return type(wum.draw.texture('hud.png', {mips = false}))"), "number", "{mips = false} loads");
    Expect(!draw::g_lastOpts.mips && draw::g_lastOpts.layers == 0 && !draw::g_lastOpts.nearest, "{mips = false}: no mips, linear, 2D");
    Eval("img", "wum.draw.texture('hud.png', {mips = false, wrap = 'repeat'})");
    Expect(!draw::g_lastOpts.mips && draw::g_lastOpts.repeat, "mips combines with wrap");
    Expect(Eval("img", "wum.draw.texture('a.png', {mips = 'no'})").find("mips must be a boolean") != std::string::npos,
           "a non-boolean mips raises");
    sandbox::UnloadMod("img");
    sandbox::UnloadMod("img2");
}

void TestDrawFonts() {
    SetMod("fnt", "");
    SetMod("fnt2", "");
    const std::wstring dir = fake::g_modsDir + L"\\fnt\\fonts";
    WriteText(dir + L"\\a.json", kFontJson);
    WriteText(dir + L"\\bad.json", R"({"format": "holo-font-1", "size": 16, "lineHeight": 40, "ascent": 30, "glyphs": {"A": [0,0,1,1,0,0,1]}})");
    WriteText(dir + L"\\old.json", R"({"format": "bmfont", "size": 16})");
    WriteText(dir + L"\\out.json", R"({"format": "holo-font-1", "size": 16, "lineHeight": 40, "ascent": 30, "glyphs": {"65": [500,0,20,28,0,0,1]}})");
    WriteText(fake::g_modsDir + L"\\fnt2\\a.json", kFontJson);
    Expect(sandbox::LoadMod("fnt") && sandbox::LoadMod("fnt2"), "font mods load");
    const int live = draw::g_liveFonts;
    ExpectEq(Eval("fnt", "f = wum.draw.font('fonts/a.png', 'fonts/a.json') return math.type(f)"), "integer", "a font loads");
    Expect(draw::g_liveFonts == live + 1, "the font is held");
    ExpectEq(Eval("fnt", "local f, e = wum.draw.font('fonts/a.png', 'fonts/bad.json') return tostring(f) .. ' ' .. e"),
             "nil cannot load font fonts/a.png: metrics: glyph key \"A\" is not a decimal code point", "a bad glyph key is refused");
    ExpectEq(Eval("fnt", "local f, e = wum.draw.font('fonts/a.png', 'fonts/old.json') return tostring(f) .. ' ' .. e"),
             "nil cannot load font fonts/a.png: metrics: \"format\" must be \"holo-font-1\"", "another format is refused");
    Expect(Eval("fnt", "local f, e = wum.draw.font('fonts/a.png', 'fonts/out.json') return e").find("outside the atlas") != std::string::npos,
           "a glyph outside the atlas is refused");
    Expect(Eval("fnt", "local f, e = wum.draw.font('fonts/huge.png', 'fonts/a.json') return e").find("at most 2048x2048") != std::string::npos,
           "an atlas over 2048 is refused");
    Expect(Eval("fnt", "local f, e = wum.draw.font('fonts/a.png', 'fonts/none.json') return e").rfind("cannot read fonts/none.json", 0) == 0,
           "a missing metrics file is reported");
    Expect(Eval("fnt", "wum.draw.font('../x.png', 'fonts/a.json')").rfind("ERR:", 0) == 0, "an escaping path raises");
    Expect(Eval("fnt", "wum.draw.font('fonts/a.png')").rfind("ERR:", 0) == 0, "both files are needed");
    ExpectEq(Eval("fnt", "for i = 2, 4 do assert(wum.draw.font('fonts/a.png', 'fonts/a.json')) end "
                         "local f, e = wum.draw.font('fonts/a.png', 'fonts/a.json') return tostring(f) .. ' ' .. e"),
             "nil too many fonts loaded", "at most 4 fonts per mod");
    Expect(draw::g_liveFonts == live + 4, "four fonts held");

    // hudText options.
    draw::g_textEx.clear();
    Eval("fnt", "wum.draw.hudText(10, 20, 'Hi', '#ffffff', 28, {font = f, shadow = '#00000080', shadowOffset = {3, 4}, "
                "align = 'center', maxWidth = 120, ellipsis = false})");
    Expect(draw::g_textEx.size() == 1, "hudText with opts takes the new path");
    if (!draw::g_textEx.empty()) {
        const draw::TextOptions& o = draw::g_textEx[0].o;
        Expect(o.font == std::stoi(Eval("fnt", "return f")) && o.shadow && o.shadowColor == 0x80000000u && o.shadowDx == 3 &&
                   o.shadowDy == 4 && o.align == draw::Align::Center && o.maxWidth == 120 && !o.ellipsis &&
                   draw::g_textEx[0].size == 28,
               "hudText opts pass through");
    }
    Eval("fnt", "wum.draw.hudText(10, 20, 'Hi', '#ffffff', 28, {shadow = false})");
    Expect(draw::g_textEx.size() == 2 && !draw::g_textEx[1].o.shadow && draw::g_textEx[1].o.font == 0 && draw::g_textEx[1].o.ellipsis &&
               draw::g_textEx[1].o.align == draw::Align::Left,
           "hudText opts defaults: the bitmap font, no shadow, left, ellipsis");
    Eval("fnt", "wum.draw.hudText(10, 20, 'Hi', '#ffffff', 28, 3)");
    Expect(draw::g_textEx.size() == 2, "a number in the 6th place is still frames (the old path)");
    for (const char* bad : {"{align = 'middle'}", "{font = 9999}", "{shadow = true}", "{shadowOffset = 2}", "{maxWidth = 'x'}",
                            "{ellipsis = 1}", "{size = 3}"})
        Expect(Eval("fnt", std::string("wum.draw.hudText(0, 0, 'x', '#ffffff', 16, ") + bad + ")").rfind("ERR:", 0) == 0,
               std::string("hudText raises for opts ") + bad);
    Expect(Eval("fnt2", "wum.draw.hudText(0, 0, 'x', '#ffffff', 16, {font = " + Eval("fnt", "return f") + "})").find("this mod's wum.draw.font") !=
               std::string::npos,
           "another mod's font raises");

    // textSize
    ExpectEq(Eval("fnt", "local w, h = wum.draw.textSize('abcd', 10) return w .. ' ' .. h"), "20.0 10.0", "textSize with the bitmap font");
    ExpectEq(Eval("fnt", "local w, h = wum.draw.textSize('abcd', 10, {font = f}) return w - f"), "20", "textSize with a mod font");
    ExpectEq(Eval("fnt", "local w = wum.draw.textSize('ab') return w"), "16", "textSize defaults to size 16");
    Expect(Eval("fnt", "wum.draw.textSize('x', 10, {bold = true})").rfind("ERR:", 0) == 0, "textSize takes only font");
    Expect(Eval("fnt", "wum.draw.textSize({}, 10)").rfind("ERR:", 0) == 0, "textSize needs a string");

    sandbox::UnloadMod("fnt");
    Expect(draw::g_liveFonts == live, "unloading frees the mod's fonts");
    sandbox::UnloadMod("fnt2");
}

void TestDrawClipAndMedia() {
    SetMod("clp", "");
    SetMod("clp2", "");
    Expect(sandbox::LoadMod("clp") && sandbox::LoadMod("clp2"), "clip/media mods load");
    draw::g_stage = render::Stage::Count;
    ExpectEq(Eval("clp", "return wum.draw.hudClip(0, 0, 10, 10)"), "false", "hudClip outside a hud/screen callback");
    draw::g_stage = render::Stage::Screen;
    draw::g_clipDepth = 0;
    ExpectEq(Eval("clp", "local r = {} for i = 1, 5 do r[i] = tostring(wum.draw.hudClip(0, 0, 10, 10)) end return table.concat(r, ',')"),
             "true,true,true,true,false", "hudClip nests 4 deep");
    Eval("clp", "for i = 1, 6 do wum.draw.hudClipEnd() end");
    Expect(draw::g_clipDepth == 0, "hudClipEnd unwinds (and extra ends are harmless)");
    Expect(Eval("clp", "wum.draw.hudClip(0, 0, 10)").rfind("ERR:", 0) == 0, "hudClip needs four numbers");
    draw::g_stage = render::Stage::Count;

    // wum.draw.media
    InjectPlayer("clp", "p", 5);
    InjectPlayer("clp", "closed", 0);
    InjectPlayer("clp", "stale", 6, -1);
    draw::g_video.clear();
    media::g_frameOk = true;
    ExpectEq(Eval("clp", "return table.concat({wum.draw.media(p, 0, 0, 1000, 1000)}, ' ')"), "0.0 290.625 1000.0 709.375",
             "contain letterboxes a 2.39:1 frame");
    ExpectEq(media::g_frameMod + " " + std::to_string(media::g_framePlayer), "clp 5", "FrameForDraw gets the calling mod and the player id");
    Expect(draw::g_video.size() == 1 && draw::g_video[0].tex == 77 && draw::g_video[0].alpha == 1.f &&
               std::fabs(draw::g_video[0].uv[3] - 536.f / 720.f) < 1e-6f,
           "the frame is drawn as video with the frame's uv");
    ExpectEq(Eval("clp", "return table.concat({wum.draw.media(p, 0, 0, 100, 100, {fit = 'cover', alpha = 0.5})}, ' ')"), "0.0 0.0 100.0 100.0",
             "cover fills the rectangle");
    Expect(draw::g_video.size() == 2 && draw::g_video[1].alpha == 0.5f && draw::g_video[1].uv[0] > 0.29f && draw::g_video[1].uv[2] < 0.71f,
           "cover crops the texture rectangle; alpha passes");
    ExpectEq(Eval("clp", "return table.concat({wum.draw.media(p, 0, 0, 100, 50, {fit = 'stretch'})}, ' ')"), "0.0 0.0 100.0 50.0",
             "stretch fills the rectangle");
    const int calls = media::g_frameCalls;
    ExpectEq(Eval("clp", "return tostring(wum.draw.media(closed, 0, 0, 1, 1))"), "nil", "a closed player draws nothing");
    ExpectEq(Eval("clp", "return tostring(wum.draw.media(stale, 0, 0, 1, 1))"), "nil", "a player from an older generation draws nothing");
    Expect(media::g_frameCalls == calls, "neither asks the media host");
    media::g_frameOk = false;
    ExpectEq(Eval("clp", "return tostring(wum.draw.media(p, 0, 0, 1, 1))"), "nil", "nil without a frame (or another mod's player)");
    media::g_frameOk = true;
    ExpectEq(Eval("clp", "return tostring(wum.draw.media(p, 5, 5, 5, 9))"), "nil", "nil for an empty rectangle");
    Expect(Eval("clp", "wum.draw.media({}, 0, 0, 1, 1)").find("wum.media player expected") != std::string::npos, "a non-player raises");
    Expect(Eval("clp", "wum.draw.media(p, 0, 0, 1, 1, {fit = 'zoom'})").rfind("ERR:", 0) == 0, "an unknown fit raises");
    Expect(Eval("clp", "wum.draw.media(p, 0, 0, 1, 1, {alpha = 'half'})").rfind("ERR:", 0) == 0, "a non-number alpha raises");
    Expect(Eval("clp", "wum.draw.media(p, 0, 0, 1, 1, {fit = 'cover', scale = 2})").rfind("ERR:", 0) == 0, "an unknown option raises");
    sandbox::UnloadMod("clp");
    sandbox::UnloadMod("clp2");
}

void TestDrawScreen() {
    TestDrawScreenStage();
    TestDrawImages();
    TestDrawFonts();
    TestDrawClipAndMedia();
}
