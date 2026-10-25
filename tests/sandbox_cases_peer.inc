// sandbox_selftest cases for wum.lobby / wum.peer (owner: C3). Included in the test file's anonymous namespace after
// the helpers; defines TestPeer(), run before the docs check. C0's smoke cases only.
void TestPeer() {
    ExpectEq(Eval(nullptr, "return tostring(wum.lobby.state())"), "nil", "no lobby outside a net session");
    ExpectEq(Eval(nullptr, "return tostring(wum.lobby.isHost())"), "false", "isHost is false outside a session");
    ExpectEq(Eval(nullptr, "local a, b = wum.peer.now(), wum.peer.now() return tostring(math.type(a) == 'float' and b > a)"), "true",
             "wum.peer.now() is a rising float");
}
