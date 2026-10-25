// sandbox_selftest cases for wum.game.turn/ui and the melange.turn/ui events (owner: C1). Included in the test file's
// anonymous namespace after the helpers; defines TestGameTurn(), run before the docs check. The derivation of the
// fields (phase, "mine", the latch, the change detection) is covered by gamestate_selftest against turn_logic.h; the
// sandbox only publishes what sandbox_module.cpp stored in Game().
void TestGameTurn() {
    ExpectEq(Eval(nullptr, "return type(wum.game.ui)"), "function", "wum.game.ui exists");
    sandbox::GameState& g = sandbox::Game();
    const sandbox::GameState saved = g;

    g = sandbox::GameState();
    g.turn = 3;
    ExpectEq(Eval(nullptr, "local t = wum.game.turn(); return t.index .. ',' .. tostring(t.team) .. ',' .. tostring(t.worm) .. ',' .. "
                           "tostring(t.ai) .. ',' .. tostring(t.mine) .. ',' .. t.phase"),
             "3,nil,nil,false,false,none", "between turns: team and worm absent, phase none");
    g.team = 2;
    g.worm = 5;
    g.ai = false;
    g.mine = true;
    g.phase = "play";
    ExpectEq(Eval(nullptr, "local t = wum.game.turn(); return t.index .. ',' .. t.team .. ',' .. t.worm .. ',' .. tostring(t.ai) .. ',' .. "
                           "tostring(t.mine) .. ',' .. t.phase"),
             "3,2,5,false,true,play", "a turn of mine");
    g.team = 0;
    g.worm = 0;
    g.ai = true;
    g.mine = false;
    g.phase = "retreat";
    ExpectEq(Eval(nullptr, "local t = wum.game.turn(); return t.team .. ',' .. t.worm .. ',' .. tostring(t.ai) .. ',' .. tostring(t.mine) .. ',' .. t.phase"),
             "0,0,true,false,retreat", "team and worm 0 are present (0 is a value)");
    Expect(Eval(nullptr, "wum.game.turn().phase = 'x'; return 1") == "1", "the returned table is a copy");
    ExpectEq(Eval(nullptr, "return wum.game.turn().phase"), "retreat", "and changing it changes nothing");

    ExpectEq(Eval(nullptr, "return tostring(wum.game.ui())"), "nil", "ui is nil on an unrecognised build");
    g.ui.valid = true;
    g.ui.pause = true;
    g.ui.attract = true;
    ExpectEq(Eval(nullptr, "local u = wum.game.ui(); return tostring(u.weaponPanel) .. ',' .. tostring(u.pause) .. ',' .. tostring(u.readyIntro) .. ',' .. "
                           "tostring(u.feCursor) .. ',' .. tostring(u.attract)"),
             "false,true,false,false,true", "ui carries the five flags");
    g.ui.weaponPanel = true;
    g.ui.readyIntro = true;
    g.ui.feCursor = true;
    ExpectEq(Eval(nullptr, "local u = wum.game.ui(); return tostring(u.weaponPanel) .. ',' .. tostring(u.readyIntro) .. ',' .. tostring(u.feCursor)"),
             "true,true,true", "and follows the stored state");

    g.online = true;
    ExpectEq(Eval(nullptr, "return tostring(wum.game.online())"), "true", "online follows Game().online");
    g = saved;
}
