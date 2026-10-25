// sandbox_selftest cases for wum.game.turn/ui and the melange.turn/ui events (owner: C1). Included in the test file's
// anonymous namespace after the helpers; defines TestGameTurn(), run before the docs check. C0's smoke cases only.
void TestGameTurn() {
    ExpectEq(Eval(nullptr, "return type(wum.game.ui)"), "function", "wum.game.ui exists");
}
