// Offline self-test of the console's pure logic: REPL text helpers and command history.
#include <iostream>

#include "lua/console_history.h"
#include "lua/console_repl.h"

namespace {
using melange::console::CompletionWord;
using melange::console::ExpandShorthand;
using melange::console::History;
using melange::console::SpliceCompletion;

int g_checks = 0, g_failures = 0;
void Check(bool cond, const char* what) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
}

void TestShorthand() {
    Check(ExpandShorthand("=1+1") == "return 1+1", "shorthand: '=1+1' -> 'return 1+1'");
    Check(ExpandShorthand("  =foo() ") == "return foo()", "shorthand: trims whitespace around '='");
    Check(ExpandShorthand("print(1)") == "print(1)", "shorthand: a plain statement passes through");
    Check(ExpandShorthand("return 1+1") == "return 1+1", "shorthand: an existing 'return' passes through");
    Check(ExpandShorthand("") == "", "shorthand: empty stays empty");
}

void TestCompletionWord() {
    Check(CompletionWord("local x = wum.dr", 16) == "wum.dr", "completion word: dotted chain at the cursor");
    Check(CompletionWord("wum.draw.hud", 8) == "wum.draw", "completion word: cursor mid-chain");
    Check(CompletionWord("  ", 2) == "", "completion word: whitespace has no word");
    Check(CompletionWord("foo", 0) == "", "completion word: cursor at 0 has no word before it");
    Check(CompletionWord("a.b.c", 5) == "a.b.c", "completion word: whole chain when it starts the line");
    Check(CompletionWord("1 + wum", 7) == "wum", "completion word: stops at the operator before it");
}

void TestSpliceCompletion() {
    Check(SpliceCompletion("wum.dr", "draw") == "wum.draw", "splice: replaces the last segment");
    Check(SpliceCompletion("dr", "draw") == "draw", "splice: no separator replaces the whole word");
    Check(SpliceCompletion("a.b:c", "call") == "a.b:call", "splice: keeps everything up to the last ':' or '.'");
}

void TestHistoryNavigation() {
    History h(3);
    Check(h.Size() == 0, "history: starts empty");
    h.Add("one");
    h.Add("two");
    h.Add("three");
    Check(h.Size() == 3, "history: holds up to its capacity");
    h.Add("four");
    Check(h.Size() == 3, "history: caps at capacity");
    Check(h.At(0) == "two" && h.At(2) == "four", "history: drops the oldest entry once full");

    h.ResetCursor();
    const std::string* p = h.Older("draft");
    Check(p && *p == "four", "history: Up recalls the newest entry first");
    p = h.Older("draft");
    Check(p && *p == "three", "history: a second Up recalls the entry before it");
    p = h.Older("draft");
    Check(p && *p == "two", "history: a third Up reaches the oldest entry");
    p = h.Older("draft");
    Check(!p, "history: Up at the oldest entry returns null");
    p = h.Newer();
    Check(p && *p == "three", "history: Down moves back toward the newest entry");
    p = h.Newer();
    Check(p && *p == "four", "history: Down reaches the newest entry");
    p = h.Newer();
    Check(p && *p == "draft", "history: one more Down returns the saved draft");
    p = h.Newer();
    Check(!p, "history: Down past the draft returns null");
}

void TestHistoryDedup() {
    History h(10);
    h.Add("same");
    h.Add("same");
    Check(h.Size() == 1, "history: a repeat of the last entry is not added again");
    h.Add("");
    Check(h.Size() == 1, "history: an empty line is not added");
}

void TestHistorySerialize() {
    History h(10);
    h.Add("print(1)");
    h.Add("for i=1,3 do\nprint(i)\nend");
    h.Add("a\\b");
    std::string text = h.Serialize();
    History h2(10);
    h2.Load(text);
    Check(h2.Size() == 3, "history: round-trips the same number of entries");
    Check(h2.At(0) == "print(1)", "history: a plain entry round-trips");
    Check(h2.At(1) == "for i=1,3 do\nprint(i)\nend", "history: embedded newlines round-trip");
    Check(h2.At(2) == "a\\b", "history: an embedded backslash round-trips");
}

void TestHistoryCapOnLoad() {
    History h(2);
    h.Load("a\nb\nc\n");
    Check(h.Size() == 2, "history: loading more than capacity keeps only the newest entries");
    Check(h.At(0) == "b" && h.At(1) == "c", "history: load trims from the oldest end");
}
}  // namespace

int main() {
    TestShorthand();
    TestCompletionWord();
    TestSpliceCompletion();
    TestHistoryNavigation();
    TestHistoryDedup();
    TestHistorySerialize();
    TestHistoryCapOnLoad();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
