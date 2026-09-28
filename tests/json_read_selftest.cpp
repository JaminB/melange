// Offline self-test for src/tools/json_read (no game needed). Exit code 0 = all passed.
#include <cstdio>
#include <cstring>
#include <string>

#include "tools/json_read.h"

using namespace melange::json;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what);
    }
}

void Valid(const char* text, const char* what) {
    Value v;
    Error e;
    const bool ok = Parse(text, &v, &e);
    if (!ok) printf("  %s: %d:%d %s\n", what, e.line, e.col, e.text.c_str());
    Expect(ok, what);
}

void Invalid(std::string_view text, int line, int col, const char* contains, const char* what) {
    Value v;
    Error e;
    const bool ok = Parse(text, &v, &e);
    const bool good = !ok && e.line == line && e.col == col && e.text.find(contains) != std::string::npos;
    if (!good) printf("  %s: got %s %d:%d \"%s\"\n", what, ok ? "ok" : "error", e.line, e.col, e.text.c_str());
    Expect(good, what);
}
}  // namespace

int main() {
    Valid("{}", "empty object");
    Valid("[]", "empty array");
    Valid("  null ", "null with whitespace");
    Valid("\xEF\xBB\xBF{\"a\":1}", "UTF-8 BOM");
    Valid("[0, -0, 1.5, -2e10, 3E+2, 4e-2, 123456789]", "numbers");
    Valid("\"caf\xC3\xA9 \xF0\x9F\x8C\xB6\"", "UTF-8 in strings");
    Valid("{\"a\":{\"b\":[true,false,null,{\"c\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}]}}", "nesting and escapes");

    {
        Value v;
        Error e;
        const char* doc =
            "{\n"
            "  \"spiceVersion\": 1,\n"
            "  \"id\": \"hello-spice\",\n"
            "  \"authors\": [\"A\", \"B\"],\n"
            "  \"entry\": { \"client\": \"client/init.lua\" },\n"
            "  \"ratio\": 0.25,\n"
            "  \"u\": \"\\u00e9\\ud83c\\udf36\"\n"
            "}\n";
        const bool ok = Parse(doc, &v, &e);
        Expect(ok && v.IsObject() && v.members.size() == 6, "manifest: parses");
        Expect(ok && v.members[1].first == "id", "manifest: document order");
        const Value* id = v.Get("id");
        Expect(id && id->IsString() && id->string == "hello-spice" && id->line == 3 && id->col == 9, "manifest: id + position");
        const Value* sv = v.Get("spiceVersion");
        Expect(sv && sv->IsInteger() && sv->number == 1, "manifest: integer");
        const Value* r = v.Get("ratio");
        Expect(r && r->IsNumber() && !r->IsInteger() && r->number == 0.25, "manifest: fraction");
        const Value* a = v.Get("authors");
        Expect(a && a->IsArray() && a->items.size() == 2 && a->items[1].string == "B" && a->items[1].line == 4,
               "manifest: array");
        const Value* en = v.Get("entry");
        Expect(en && en->Get("client") && en->Get("client")->string == "client/init.lua", "manifest: nested get");
        const Value* u = v.Get("u");
        Expect(u && u->string == "\xC3\xA9\xF0\x9F\x8C\xB6", "manifest: \\u escapes and surrogate pair");
        Expect(!v.Get("missing") && !id->Get("x"), "manifest: missing keys");
    }

    Invalid("", 1, 1, "unexpected end", "empty input");
    Invalid("{", 1, 2, "unexpected end", "open object");
    Invalid("{\"a\" 1}", 1, 6, "expected ':'", "missing colon");
    Invalid("{\"a\":1,}", 1, 8, "expected a string key", "trailing comma in object");
    Invalid("[1,]", 1, 4, "unexpected character ']'", "trailing comma in array");
    Invalid("[1 2]", 1, 4, "expected ',' or ']'", "missing comma");
    Invalid("{\n  \"a\": 1,\n  \"a\": 2\n}", 3, 3, "duplicate key \"a\"", "duplicate key");
    Invalid("{\n  \"a\": tru\n}", 2, 8, "unexpected character 't'", "bad literal");
    Invalid("[01]", 1, 3, "leading zeros", "leading zero");
    Invalid("[1.]", 1, 4, "digit after '.'", "bare decimal point");
    Invalid("[1e]", 1, 4, "exponent", "bare exponent");
    Invalid("[1e999]", 1, 2, "out of range", "number overflow");
    Invalid("[-]", 1, 3, "unexpected character ']'", "lone minus");
    Invalid("\"abc", 1, 5, "unterminated string", "unterminated string");
    Invalid("\"a\nb\"", 1, 3, "unterminated string", "newline in string");
    Invalid("\"a\tb\"", 1, 3, "control character", "tab in string");
    Invalid("\"\\x\"", 1, 2, "invalid escape", "bad escape");
    Invalid("\"\\u12g4\"", 1, 6, "hex digits", "bad \\u");
    Invalid("\"\\ud800\"", 1, 8, "unpaired surrogate", "lone high surrogate");
    Invalid("\"\\udc00\"", 1, 8, "unpaired surrogate", "lone low surrogate");
    Invalid("\"\xC3\"", 1, 2, "invalid UTF-8", "truncated UTF-8");
    Invalid("\"\xC0\xAF\"", 1, 2, "invalid UTF-8", "overlong UTF-8");
    Invalid("\"\xED\xA0\x80\"", 1, 2, "invalid UTF-8", "encoded surrogate");
    Invalid("{} x", 1, 4, "after the top-level value", "trailing content");
    Invalid("// c\n{}", 1, 1, "unexpected character '/'", "comments are not JSON");
    Invalid("{'a':1}", 1, 2, "expected a string key", "single quotes");
    Invalid(std::string_view("[1,\0]", 5), 1, 4, "unexpected byte 0x00", "NUL byte");

    {
        std::string deep(kMaxDepth + 2, '[');
        deep += std::string(kMaxDepth + 2, ']');
        Value v;
        Error e;
        Expect(!Parse(deep, &v, &e) && e.text.find("nesting") != std::string::npos, "depth limit");
        std::string ok(kMaxDepth, '[');
        ok += std::string(kMaxDepth, ']');
        Expect(Parse(ok, &v, &e), "depth at the limit");
    }
    {
        std::string big = "\"" + std::string(kMaxBytes, 'x') + "\"";
        Value v;
        Error e;
        Expect(!Parse(big, &v, &e) && e.text.find("larger than") != std::string::npos, "1 MB cap");
        Expect(Parse(big, &v, &e, big.size()), "cap is configurable");
    }
    {
        Value v;
        Error e;
        Expect(!ParseFile(L"Z:\\does\\not\\exist.json", &v, &e) && !e.text.empty(), "missing file");
    }

    printf("json_read_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
