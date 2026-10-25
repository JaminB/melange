// sandbox_selftest cases for the Melange 0.12 wum.input functions (owner: C2). Included in the test file's anonymous
// namespace after the helpers; defines TestModInput(), run before the docs check. C0's smoke cases only.
void TestModInput() {
    for (const char* f : {"cursor", "region", "hover", "pointer", "pointerActive", "focus"})
        ExpectEq(Eval(nullptr, std::string("return type(wum.input.") + f + ")"), "function", std::string("wum.input.") + f + " exists");
}
