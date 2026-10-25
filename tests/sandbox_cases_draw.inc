// sandbox_selftest cases for the Melange 0.12 wum.draw functions (owner: C5). Included in the test file's anonymous
// namespace after the helpers; defines TestDrawScreen(), run before the docs check. C0's smoke cases only.
void TestDrawScreen() {
    for (const char* f : {"hudImageUV", "hudNine", "font", "textSize", "hudClip", "hudClipEnd", "media"})
        ExpectEq(Eval(nullptr, std::string("return type(wum.draw.") + f + ")"), "function", std::string("wum.draw.") + f + " exists");
}
