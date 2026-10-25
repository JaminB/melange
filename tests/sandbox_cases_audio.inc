// sandbox_selftest cases for wum.audio.music (owner: C1). Included in the test file's anonymous namespace after the
// helpers; defines TestMusic(), run before the docs check. C0's smoke cases only.
void TestMusic() {
    ExpectEq(Eval(nullptr, "return type(wum.audio.music.suppress)"), "function", "wum.audio.music exists");
    Expect(Eval(nullptr, "wum.audio.music.suppress = nil").rfind("ERR:", 0) == 0, "wum.audio.music is read-only");
}
