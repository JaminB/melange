// sandbox_selftest cases for wum.media (owner: C4b). Included in the test file's anonymous namespace after the helpers
// (SetMod, Eval, Frames, Expect, ExpectEq, StatusOf); defines TestMedia(), run before the docs check.
// C0's smoke cases: the namespace exists, classify is complete, the rest reports "unavailable".
void TestMedia() {
    ExpectEq(Eval(nullptr, "return type(wum.media)"), "table", "wum.media exists");
    ExpectEq(Eval(nullptr, "local c = wum.media.classify(' https://youtu.be/dQw4w9WgXcQ?t=3 ') return c.kind .. ' ' .. c.id .. ' ' .. c.url"),
             "youtube dQw4w9WgXcQ https://www.youtube.com/watch?v=dQw4w9WgXcQ", "classify: a youtu.be link");
    ExpectEq(Eval(nullptr, "local c = wum.media.classify('magnet:?xt=urn:btih:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA&ws=http://x/') "
                           "return c.kind .. ' ' .. c.infohash .. ' ' .. c.url"),
             "magnet 0000000000000000000000000000000000000000 magnet:?xt=urn:btih:0000000000000000000000000000000000000000",
             "classify: a base32 magnet, ws dropped");
    ExpectEq(Eval(nullptr, "local a, b = wum.media.classify('http://youtu.be/dQw4w9WgXcQ') return tostring(a) .. ':' .. b"),
             "nil:unsupported-url", "classify: http is refused");
    Expect(Eval(nullptr, "return pcall(wum.media.classify, {})").rfind("false", 0) == 0, "classify: a non-string raises");
    ExpectEq(Eval(nullptr, "local a, b = wum.media.available() return tostring(a) .. ':' .. tostring(b)"), "false:unavailable",
             "available() before the media host exists");
}
