// Offline self-test for the standalone-only file logic: no game, no server, no network.
//   ini_edit: reading and rewriting one key of Melange.ini without disturbing the rest of the file.
//   mods_provider: mods.list / mods.setEnabled against real spice.json files and thumper-state.json.
// Exit code 0 = all passed.
#include <windows.h>

#include <cstdio>
#include <string>

#include "oasis/standalone/ini_edit.h"
#include "oasis/standalone/mods_provider.h"
#include "tools/json_read.h"

namespace ini = melange::oasis::standalone::ini;
namespace modsprov = melange::oasis::standalone::modsprov;
namespace json = melange::json;

namespace {
int g_pass = 0, g_fail = 0;
void Expect(bool ok, const char* what, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
    }
}

void WriteFile_(const std::wstring& path, const std::string& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------- ini_edit
void TestIni() {
    const std::string text =
        "; header comment\r\n"
        "[Oasis]\r\n"
        "Enabled=1\r\n"
        "Port=8765            ; the first port to try\r\n"
        "\r\n"
        "[Logging]\r\n"
        "Enabled=1\r\n";

    Expect(ini::Get(text, "Oasis", "Port") == "8765", "Get reads an existing key");
    Expect(ini::Get(text, "Oasis", "Nope") == "", "Get on a missing key is empty");
    Expect(ini::Get(text, "Nope", "Port") == "", "Get on a missing section is empty");

    std::string out;
    Expect(ini::Set(text, "Oasis", "Port", "9000", &out), "Set an existing key succeeds");
    Expect(out.find("Port=9000            ; the first port to try") != std::string::npos, "Set keeps the inline comment", out);
    Expect(out.find("[Logging]\r\nEnabled=1") != std::string::npos, "Set leaves the rest of the file untouched");
    Expect(ini::Get(out, "Oasis", "Port") == "9000", "Get after Set round-trips");

    Expect(ini::Set(text, "Oasis", "MaxClients", "4", &out), "Set a missing key in an existing section succeeds");
    Expect(out.find("[Oasis]\r\nMaxClients=4\r\nEnabled=1") != std::string::npos, "new key lands right after the header", out);

    Expect(ini::Set(text, "NewMod", "Setting", "x", &out), "Set into a brand-new section succeeds");
    Expect(out.find("[NewMod]\r\nSetting=x\r\n") != std::string::npos, "new section is appended", out);
    Expect(out.substr(0, text.size()) == text, "the original bytes are untouched when only appending");

    Expect(!ini::Set(text, "Oasis", "Port", "no\nbreaks", &out), "Set refuses a value with a line break");
    Expect(!ini::Set(text, "Oasis", "Port", "has;semicolon", &out), "Set refuses a value with ';'");

    const std::string noEol = "[A]\nK=1";  // LF endings, no trailing newline
    Expect(ini::Set(noEol, "A", "K", "2", &out) && out == "[A]\nK=2", "LF endings and no trailing newline are preserved", out);
}

// ---------------------------------------------------------------- mods_provider
std::wstring MakeModsFixture() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t root[MAX_PATH];
    swprintf(root, MAX_PATH, L"%soasis_f_mods_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryW(root, nullptr);
    CreateDirectoryW((std::wstring(root) + L"\\Mods").c_str(), nullptr);
    CreateDirectoryW((std::wstring(root) + L"\\Mods\\alpha").c_str(), nullptr);
    CreateDirectoryW((std::wstring(root) + L"\\Mods\\beta").c_str(), nullptr);
    auto manifest = [](const char* id, bool defaultEnabled) {
        return std::string("{\"spiceVersion\":1,\"id\":\"") + id +
               "\",\"version\":\"1.0.0\",\"name\":\"" + id + "\",\"authors\":[\"t\"],"
               "\"melange\":{\"range\":\">=0.0.0\"},\"kind\":\"client-only\",\"defaultEnabled\":" +
               (defaultEnabled ? "true" : "false") + ",\"entry\":{\"client\":\"init.lua\"}}";
    };
    WriteFile_(std::wstring(root) + L"\\Mods\\alpha\\spice.json", manifest("alpha", true));
    WriteFile_(std::wstring(root) + L"\\Mods\\alpha\\init.lua", "-- alpha\n");
    WriteFile_(std::wstring(root) + L"\\Mods\\beta\\spice.json", manifest("beta", false));
    WriteFile_(std::wstring(root) + L"\\Mods\\beta\\init.lua", "-- beta\n");
    return root;
}

void TestMods() {
    const std::wstring root = MakeModsFixture();
    std::string list = modsprov::ListJson(root, "1.0.0");
    Expect(list.find("\"id\":\"alpha\"") != std::string::npos && list.find("\"id\":\"beta\"") != std::string::npos,
           "ListJson finds both mods", list);
    Expect(list.find("\"id\":\"alpha\",\"name\":\"alpha\",\"version\":\"1.0.0\",\"authors\":\"t\",\"kind\":\"client-only\","
                     "\"state\":\"enabled\"") != std::string::npos,
           "alpha defaults to enabled", list);
    Expect(list.find("\"id\":\"beta\"") < list.find("\"state\":\"disabled\"", list.find("\"id\":\"beta\"")), "beta defaults to disabled", list);

    Expect(modsprov::SetEnabled(root, "nope", true) == 0, "SetEnabled on an unknown id fails");
    Expect(modsprov::SetEnabled(root, "beta", true) == 1, "SetEnabled turns beta on");
    list = modsprov::ListJson(root, "1.0.0");
    const size_t betaPos = list.find("\"id\":\"beta\"");
    Expect(betaPos != std::string::npos && list.find("\"state\":\"enabled\"", betaPos) != std::string::npos, "beta is now enabled", list);

    std::string stateText;
    FILE* f = _wfopen((root + L"\\Mods\\thumper-state.json").c_str(), L"rb");
    Expect(f != nullptr, "thumper-state.json was written");
    if (f) {
        char buf[4096];
        const size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        stateText.assign(buf, n);
    }
    json::Value v;
    json::Error e;
    Expect(json::Parse(stateText, &v, &e), "thumper-state.json is valid JSON", stateText);
    const json::Value* en = v.Get("enabled");
    const json::Value* betaVal = en ? en->Get("beta") : nullptr;
    Expect(betaVal && betaVal->IsBool() && betaVal->boolean, "the persisted state has enabled.beta = true");
}
}  // namespace

int main() {
    TestIni();
    TestMods();
    printf("oasis_standalone_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
