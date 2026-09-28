// Offline self-test for the Oasis panel RPCs that need no game: the Melange.ini editor behind ini.get/ini.set
// (byte-exact edits, encodings, the Deep Desert guard) and the lua.* parameter rules. Exit code 0 = all passed.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/config_schema.h"
#include "melange/oasis.h"
#include "oasis/rpc/ini_edit.h"
#include "oasis/rpc/ini_rpc.h"
#include "oasis/rpc/lua_request.h"
#include "tools/json_read.h"

namespace ini = melange::oasis::ini;
namespace rpc = melange::oasis::rpc;
namespace oa = melange::oasis;
namespace json = melange::json;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const char* what, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
    }
}

void ExpectEq(const std::string& got, const std::string& want, const char* what) {
    Expect(got == want, what, got == want ? "" : "got <" + got + "> want <" + want + ">");
}

// ---------------------------------------------------------------- the pure editor
void TestParse() {
    const std::string t =
        "; Melange settings\r\n"
        "orphan=1\r\n"
        "[Oasis]\r\n"
        "Enabled=1\r\n"
        "  Port = 8765   ; first port\r\n"
        "Quoted=\"a b\"\r\n"
        "enabled=0\r\n"
        "[oasis]\r\n"
        "Port=9999\r\n"
        "[Mod.x]\r\n"
        "name=\r\n"
        "novalue\r\n";
    const auto e = ini::Parse(t);
    Expect(e.size() == 4, "parse: first section and first key win, orphans and bare lines ignored", std::to_string(e.size()));
    const ini::Entry* port = ini::Find(e, "OASIS", "port");
    Expect(port && port->value == "8765" && port->line == 5, "parse: case-insensitive, trimmed, inline comment removed");
    const ini::Entry* q = ini::Find(e, "Oasis", "Quoted");
    Expect(q && q->value == "a b", "parse: surrounding quotes removed");
    const ini::Entry* en = ini::Find(e, "Oasis", "Enabled");
    Expect(en && en->value == "1", "parse: the first duplicate key wins");
    const ini::Entry* n = ini::Find(e, "Mod.x", "name");
    Expect(n && n->value.empty(), "parse: empty value");
    Expect(!ini::Find(e, "Oasis", "novalue") && !ini::Find(e, "", "orphan"), "parse: no phantom keys");
}

void TestSet() {
    const std::string t =
        "; header comment\r\n"
        "[Oasis]\r\n"
        "Enabled=1\r\n"
        "  MaxClients = 4   ; how many tabs\r\n"
        "\r\n"
        "; about the next section\r\n"
        "[Other]\r\n"
        "A=1\r\n";
    ExpectEq(ini::Set(t, "oasis", "maxclients", "8"),
             "; header comment\r\n[Oasis]\r\nEnabled=1\r\n  MaxClients = 8   ; how many tabs\r\n\r\n; about the next section\r\n[Other]\r\nA=1\r\n",
             "set: in place, spacing, spelling and comment kept");
    ExpectEq(ini::Set(t, "Oasis", "Port", "8765"),
             "; header comment\r\n[Oasis]\r\nEnabled=1\r\n  MaxClients = 4   ; how many tabs\r\nPort=8765\r\n\r\n; about the next section\r\n[Other]\r\nA=1\r\n",
             "set: a new key goes after the section's last key");
    ExpectEq(ini::Set(t, "New", "K", "v"), t + "\r\n[New]\r\nK=v\r\n", "set: a new section is appended");
    ExpectEq(ini::Set("[A]\nx=1\n", "A", "x", ""), "[A]\nx=\n", "set: LF files stay LF, empty values");
    ExpectEq(ini::Set("[A]\nx=1", "A", "y", "2"), "[A]\nx=1\ny=2", "set: no trailing newline at end of file");
    ExpectEq(ini::Set("[A]\nx=1 ; c", "A", "x", ""), "[A]\nx=; c", "set: empty value keeps the comment");
    ExpectEq(ini::Set("", "A", "x", "1"), "[A]\nx=1\n", "set: empty file");
    ExpectEq(ini::Set("[A]", "A", "x", "1"), "[A]\nx=1", "set: section header only");
    ExpectEq(ini::Set("[A]\r\n[B]\r\nx=1\r\n", "A", "x", "2"), "[A]\r\nx=2\r\n[B]\r\nx=1\r\n", "set: the right section only");
    const std::string big = ini::Set(t, "Oasis", "Enabled", "0");
    Expect(big.size() == t.size() && big.find("Enabled=0") != std::string::npos, "set: one byte changes, nothing else");
}

void TestValidation() {
    std::string why;
    Expect(ini::ValidName("Oasis", &why) && ini::ValidName("Mod.hello-spice", &why), "names: normal");
    for (const char* bad : {"", " x", "x ", "a=b", "a]", "[a", "a;b", "a\nb", "a\rb"})
        Expect(!ini::ValidName(bad, &why), "names: refused", bad);
    Expect(!ini::ValidName(std::string(129, 'a'), &why), "names: too long");
    Expect(ini::ValidValue("", &why) && ini::ValidValue("Ctrl+Shift+O", &why) && ini::ValidValue("a b\tc", &why), "values: normal");
    for (const char* bad : {"a\nb", "a\rb", "a;b", " x", "x ", "\x01"}) Expect(!ini::ValidValue(bad, &why), "values: refused", bad);
    Expect(!ini::ValidValue(std::string("a\0b", 3), &why), "values: NUL refused");
    Expect(!ini::ValidValue(std::string(1001, 'a'), &why), "values: too long");

    Expect(ini::Protected("Thumper", "AutoGrantDeepDesert", "1", &why), "deep desert: auto-grant refused");
    Expect(ini::Protected("thumper", "autograntdeepdesert", "yes", &why), "deep desert: any case, any non-zero value");
    Expect(ini::Protected("Thumper", "AutoGrantDeepDesert", "", &why), "deep desert: empty refused");
    Expect(!ini::Protected("Thumper", "AutoGrantDeepDesert", "0", &why), "deep desert: turning auto-grant off is allowed");
    Expect(ini::Protected("Thumper", "GrantSalt", "x", &why), "deep desert: salt refused");
    Expect(!ini::Protected("Oasis", "GrantSalt", "x", &why) && !ini::Protected("Thumper", "ModsDir", "Mods", &why),
           "deep desert: other keys allowed");
}

void TestEncoding() {
    ini::Encoding e;
    ExpectEq(ini::Decode("[A]\r\nx=1\r\n", &e), "[A]\r\nx=1\r\n", "decode: ansi");
    Expect(e == ini::Encoding::Ansi, "decode: ansi detected");
    ExpectEq(ini::Decode("\xEF\xBB\xBF[A]", &e), "[A]", "decode: utf-8 bom stripped");
    Expect(e == ini::Encoding::Utf8Bom, "decode: utf-8 detected");
    const std::string u16("\xFF\xFE[\0A\0]\0", 8);
    ExpectEq(ini::Decode(u16, &e), "[A]", "decode: utf-16");
    std::string out;
    Expect(e == ini::Encoding::Utf16Le && ini::Encode("[A]", e, &out) && out == u16, "encode: utf-16 round trip");
    Expect(ini::Encode("x=1", ini::Encoding::Utf8Bom, &out) && out == "\xEF\xBB\xBFx=1", "encode: utf-8 bom kept");
    Expect(ini::Encode("x=abc", ini::Encoding::Ansi, &out) && out == "x=abc", "encode: ansi");
    if (GetACP() != CP_UTF8)
        Expect(!ini::Encode("x=\xE2\x9B\x84\xF0\x9F\x8C\xB5", ini::Encoding::Ansi, &out), "encode: characters the code page lacks are refused");
    Expect(!ini::Encode("\xC3\x28", ini::Encoding::Ansi, &out), "encode: invalid UTF-8 refused");
}

// ---------------------------------------------------------------- lua.* parameters
bool Lua(const char* params, const char* key, rpc::LuaRequest* q, std::string* why) {
    json::Value v;
    json::Error e;
    if (!json::Parse(params, &v, &e)) return false;
    return rpc::ParseLua(v, key, key == std::string("code") ? rpc::kMaxCode : rpc::kMaxPrefix, q, why);
}

void TestLuaParams() {
    rpc::LuaRequest q;
    std::string why;
    Expect(Lua(R"({"code":"return 1"})", "code", &q, &why) && q.target == rpc::LuaTarget::Client && q.text == "return 1",
           "lua: target defaults to client");
    Expect(Lua(R"({"target":"match","code":"x"})", "code", &q, &why) && q.target == rpc::LuaTarget::Match, "lua: match");
    Expect(Lua(R"({"target":"mod","mod":"hello","prefix":"wum."})", "prefix", &q, &why) && q.mod == "hello" && q.text == "wum.",
           "lua: mod with prefix");
    Expect(!Lua(R"({"target":"mod","code":"x"})", "code", &q, &why), "lua: mod needs an id");
    Expect(!Lua(R"({"target":"client","mod":"x","code":"x"})", "code", &q, &why), "lua: mod only with target mod");
    Expect(!Lua(R"({"target":"server","code":"x"})", "code", &q, &why), "lua: unknown target");
    Expect(!Lua(R"({"target":1,"code":"x"})", "code", &q, &why), "lua: target type");
    Expect(!Lua(R"({"target":"client"})", "code", &q, &why), "lua: code required");
    Expect(!Lua(R"({"code":5})", "code", &q, &why), "lua: code type");
    Expect(!Lua(R"({"code":"a\u0000b"})", "code", &q, &why), "lua: NUL refused");
    const std::string big = "{\"code\":\"" + std::string(rpc::kMaxCode + 1, 'x') + "\"}";
    Expect(!Lua(big.c_str(), "code", &q, &why) && why.find("64 KB") != std::string::npos, "lua: code over 64 KB refused", why);
    const std::string ok = "{\"code\":\"" + std::string(rpc::kMaxCode, 'x') + "\"}";
    Expect(Lua(ok.c_str(), "code", &q, &why), "lua: exactly 64 KB accepted");
    const std::string clipped = rpc::ClipText(std::string(10, 'a') + "\xC3\xA9", 11);
    ExpectEq(clipped.substr(0, 10), std::string(10, 'a'), "lua: clip keeps whole characters");
    Expect(clipped.find('\xC3') == std::string::npos && clipped.find("truncated") != std::string::npos, "lua: clip marks truncation");
    ExpectEq(rpc::ClipText("short"), "short", "lua: short text untouched");
}

// ---------------------------------------------------------------- ini.get / ini.set on a real file
std::wstring g_path;

std::string ReadAll() {
    std::string s;
    if (FILE* f = _wfopen(g_path.c_str(), L"rb")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        fclose(f);
    }
    return s;
}

void WriteAll(const std::string& s) {
    if (FILE* f = _wfopen(g_path.c_str(), L"wb")) {
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
}

oa::Result Call(void (*fn)(const oa::Call&, oa::Result&, void*), const std::string& params) {
    oa::Result r;
    fn(oa::Call{"test", params, 1}, r, nullptr);
    return r;
}

void TestRpc() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    g_path = std::wstring(dir) + L"oasis_rpc_selftest_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    melange::config::Init(g_path);
    const std::string original =
        "; Melange.ini\r\n"
        "[Oasis]\r\n"
        "Enabled=1\r\n"
        "MaxClients=4 ; tabs\r\n"
        "\r\n"
        "[Thumper]\r\n"
        "GrantSalt=s3cr3t\r\n"
        "AutoGrantDeepDesert=0\r\n"
        "[Extra]\r\n"
        "Undeclared=yes\r\n";
    WriteAll(original);
    melange::config::schema::Record("Oasis", "Enabled", "1");
    melange::config::schema::Record("Oasis", "MaxClients", "4");
    melange::config::schema::Record("Oasis", "Port", "8765");
    melange::config::schema::Record("FrameInterval", "IntervalMs", "16");
    melange::config::schema::MarkLive("FrameInterval", "IntervalMs");
    melange::config::schema::Record("Thumper", "GrantSalt", "");
    melange::config::schema::Record("Thumper", "AutoGrantDeepDesert", "0");

    oa::Result r = Call(&rpc::IniGet, "{}");
    json::Value v;
    json::Error e;
    Expect(r.ok && json::Parse(r.json, &v, &e) && v.IsObject(), "ini.get: a JSON object", r.message);
    const json::Value* text = v.Get("text");
    Expect(text && text->IsString() && text->string.find("s3cr3t") == std::string::npos && text->string.find("GrantSalt=********") != std::string::npos,
           "ini.get: the grant salt is masked in the text");
    Expect(text && text->string.find("MaxClients=4 ; tabs") != std::string::npos, "ini.get: raw text");
    const json::Value* keys = v.Get("keys");
    bool sawPort = false, sawUndeclared = false, sawSalt = false, sawLive = false;
    for (const json::Value& k : keys ? keys->items : std::vector<json::Value>{}) {
        const std::string s = k.Get("section")->string, n = k.Get("key")->string;
        if (s == "Oasis" && n == "Port") sawPort = k.Get("current")->IsNull() && k.Get("def")->string == "8765" && k.Get("declared")->boolean;
        if (s == "Extra" && n == "Undeclared") sawUndeclared = k.Get("def")->IsNull() && !k.Get("declared")->boolean && k.Get("current")->string == "yes";
        if (s == "Thumper" && n == "GrantSalt") sawSalt = k.Get("current")->string == "********";
        if (s == "FrameInterval" && n == "IntervalMs") sawLive = k.Get("live")->boolean;
    }
    Expect(sawPort, "ini.get: a declared key missing from the file has current null");
    Expect(sawUndeclared, "ini.get: undeclared keys in the file are listed");
    Expect(sawSalt, "ini.get: the grant salt is masked in keys");
    Expect(sawLive, "ini.get: live flag from the schema");

    r = Call(&rpc::IniSet, R"({"section":"Oasis","key":"MaxClients","value":"6"})");
    Expect(r.ok && r.json.find("\"restart\":true") != std::string::npos && r.json.find("\"changed\":true") != std::string::npos,
           "ini.set: restart key", r.json + r.message);
    std::string want = original;
    want.replace(want.find("MaxClients=4"), 12, "MaxClients=6");
    ExpectEq(ReadAll(), want, "ini.set: only the value changed, comments and bytes kept");
    Expect(melange::config::GetInt("Oasis", "MaxClients", 0) == 6, "ini.set: GetPrivateProfileString sees the new value");

    r = Call(&rpc::IniSet, R"({"section":"Oasis","key":"MaxClients","value":"6"})");
    Expect(r.ok && r.json.find("\"changed\":false") != std::string::npos, "ini.set: same value is a no-op");
    r = Call(&rpc::IniSet, R"({"section":"FrameInterval","key":"IntervalMs","value":"8"})");
    Expect(r.ok && r.json.find("\"live\":true") != std::string::npos && r.json.find("\"restart\":false") != std::string::npos,
           "ini.set: live key", r.json + r.message);
    Expect(ReadAll().find("\r\n[FrameInterval]\r\nIntervalMs=8\r\n") != std::string::npos, "ini.set: declared key in a new section");
    r = Call(&rpc::IniSet, R"({"section":"Mod.hello","key":"color","value":"red"})");
    Expect(r.ok && r.json.find("\"live\":true") != std::string::npos, "ini.set: mod settings are live", r.message);

    const std::string before = ReadAll();
    r = Call(&rpc::IniSet, R"({"section":"Thumper","key":"AutoGrantDeepDesert","value":"1"})");
    Expect(!r.ok && r.code == -32000, "ini.set: Deep Desert auto-grant refused");
    r = Call(&rpc::IniSet, R"({"section":"thumper","key":"GRANTSALT","value":"x"})");
    Expect(!r.ok && r.code == -32000, "ini.set: grant salt refused");
    r = Call(&rpc::IniSet, R"({"section":"Oasis","key":"Nope","value":"1"})");
    Expect(!r.ok && r.code == -32602, "ini.set: unknown key refused");
    r = Call(&rpc::IniSet, R"({"section":"Oasis","key":"Port","value":"1\n[Thumper]"})");
    Expect(!r.ok && r.code == -32602, "ini.set: line break refused");
    r = Call(&rpc::IniSet, R"({"section":"Oasis","key":"Port"})");
    Expect(!r.ok && r.code == -32602, "ini.set: value required");
    r = Call(&rpc::IniSet, R"({"section":"Oasis","key":"Port","value":7})");
    Expect(!r.ok && r.code == -32602, "ini.set: value must be a string");
    r = Call(&rpc::IniSet, "[1]");
    Expect(!r.ok && r.code == -32602, "ini.set: params must be an object");
    ExpectEq(ReadAll(), before, "ini.set: refusals leave the file untouched");
    r = Call(&rpc::IniSet, R"({"section":"Thumper","key":"AutoGrantDeepDesert","value":"0"})");
    Expect(r.ok, "ini.set: auto-grant can stay off", r.message);

    WriteAll("\xFF\xFE" + std::string("[\0A\0]\0\r\0\n\0x\0=\0" "1\0\r\0\n\0", 20));
    melange::config::schema::Record("A", "x", "1");
    r = Call(&rpc::IniSet, R"({"section":"A","key":"x","value":"2"})");
    Expect(r.ok && ReadAll() == "\xFF\xFE" + std::string("[\0A\0]\0\r\0\n\0x\0=\0" "2\0\r\0\n\0", 20), "ini.set: UTF-16 files stay UTF-16",
           r.message);

    DeleteFileW(g_path.c_str());
    r = Call(&rpc::IniGet, "{}");
    Expect(r.ok && r.json.find("\"text\":\"\"") != std::string::npos, "ini.get: a missing file is empty", r.message);
    r = Call(&rpc::IniSet, R"({"section":"Oasis","key":"Port","value":"8770"})");
    ExpectEq(ReadAll(), "[Oasis]\nPort=8770\n", "ini.set: creates a missing file");
    Expect(GetFileAttributesW((g_path + L".oasis-tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "ini.set: no temp file left");
    DeleteFileW(g_path.c_str());
}
}  // namespace

int main() {
    TestParse();
    TestSet();
    TestValidation();
    TestEncoding();
    TestLuaParams();
    TestRpc();
    printf("oasis_rpc_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
