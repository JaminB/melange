// holo_media_logic_test: the helper's pure parts (no libmpv, no pipe, no network): the command line, the hardened mpv
// and yt-dlp options, the frame fit, the yt-dlp JSON reader, the resolve result and error mapping, argument quoting.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

#include "args.h"
#include "frame_size.h"
#include "json_lite.h"
#include "mpv_opts.h"
#include "resolve_logic.h"
#include "util.h"

namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;

namespace {
int g_fail = 0, g_pass = 0;
void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

bool Parse(std::initializer_list<const wchar_t*> a, holo::Args* out = nullptr) {
    std::vector<const wchar_t*> v = {L"holo-media.exe"};
    v.insert(v.end(), a.begin(), a.end());
    holo::Args tmp;
    std::string why;
    return holo::ParseArgs(static_cast<int>(v.size()), v.data(), out ? out : &tmp, &why);
}

void TestArgs() {
    holo::Args a;
    const bool ok = Parse({L"--pipe", L"\\\\.\\pipe\\melange-holo-123-00ff00ff00ff00ff", L"--parent", L"4242", L"--cache", L"C:\\c\\1-ab",
                           L"--libmpv", L"C:\\p\\libmpv-2.dll", L"--ytdlp", L"C:\\y\\yt-dlp.exe", L"--deno", L"C:\\p\\deno.exe", L"--sweep"},
                          &a);
    Check(ok && a.parent == 4242 && a.sweep && !a.selftest && a.cache == L"C:\\c\\1-ab", "full launch line parses");
    const wchar_t* base[] = {L"--parent", L"1", L"--cache", L"C:\\c", L"--libmpv", L"C:\\l.dll", L"--ytdlp", L"C:\\y.exe", L"--deno", L"C:\\d.exe"};
    auto with = [&](const wchar_t* k, const wchar_t* v) {
        std::vector<const wchar_t*> x = {L"holo-media.exe", k, v};
        x.insert(x.end(), std::begin(base), std::end(base));
        holo::Args t;
        return holo::ParseArgs(static_cast<int>(x.size()), x.data(), &t, nullptr);
    };
    Check(with(L"--pipe", L"\\\\.\\pipe\\melange-holo-1-a"), "pipe accepted");
    Check(!with(L"--pipe", L"\\\\.\\pipe\\other"), "foreign pipe name refused");
    Check(!with(L"--pipe", L"\\\\server\\pipe\\melange-holo-1-a"), "remote pipe refused");
    Check(!with(L"--pipe", L"\\\\.\\pipe\\melange-holo-1-a\\..\\x"), "pipe with path characters refused");
    Check(!Parse({L"--selftest", L"--evil", L"x"}), "unknown flag refused");
    Check(!Parse({L"--selftest", L"--cache", L"relative\\dir"}), "relative path refused");
    Check(!Parse({L"--selftest", L"--cache", L"C:\\a\\..\\b"}), "parent segment refused");
    Check(!Parse({L"--selftest", L"--cache", L"C:\\a:stream"}), "alternate data stream refused");
    Check(!Parse({L"--selftest", L"--cache", L"C:\\a", L"--cache", L"C:\\b"}), "repeated flag refused");
    Check(!Parse({L"--selftest", L"--cache"}), "missing value refused");
    Check(Parse({L"--selftest"}), "bare selftest");
    Check(!Parse({L"--selftest", L"--sweep"}), "selftest with launch flags refused");
    Check(!Parse({L"--pipe", L"\\\\.\\pipe\\melange-holo-1-a", L"--parent", L"1"}), "launch without the paths refused");
    for (const wchar_t* pid : {L"0", L"99999999999", L"12a", L"-1"})
        Check(!Parse({L"--pipe", L"\\\\.\\pipe\\melange-holo-1-a", L"--parent", pid, L"--cache", L"C:\\c", L"--libmpv", L"C:\\l.dll", L"--ytdlp",
                      L"C:\\y.exe", L"--deno", L"C:\\d.exe"}),
              "bad parent refused: " + holo::Utf8(pid));
}

void TestOptions() {
    const auto o = holo::opts::BaseOptions("C:\\c\\mpvhome", "auto-copy", 25);
    auto get = [&](const char* n) -> const holo::opts::MpvOption* {
        for (const auto& x : o)
            if (std::string(x.name) == n) return &x;
        return nullptr;
    };
    // Critique S3: every one of these must be present, and not optional.
    for (const char* n : {"config", "config-dir", "load-scripts", "osc", "input-default-bindings", "input-ipc-server", "terminal",
                          "save-position-on-quit", "resume-playback", "use-filedir-conf", "access-references", "ordered-chapters",
                          "load-unsafe-playlists", "sub-auto", "audio-file-auto", "cover-art-auto", "vo", "ytdl", "cache", "osd-level"}) {
        const auto* x = get(n);
        Check(x && !x->optional, std::string("required option ") + n);
    }
    for (const char* n : {"load-stats-overlay", "load-console", "load-select", "load-commands", "load-auto-profiles", "load-positioning",
                          "load-context-menu", "input-vo-keyboard"})
        Check(get(n) && get(n)->value == "no", std::string("script toggle ") + n);
    Check(get("config")->value == "no" && get("load-scripts")->value == "no" && get("access-references")->value == "no" &&
              get("input-ipc-server")->value.empty() && get("vo")->value == "libmpv" && get("config-dir")->value == "C:\\c\\mpvhome",
          "option values");
    Check(get("audio-delay")->value == "+0.025", "audio delay " + get("audio-delay")->value);
    Check(holo::opts::AccessReferences(false) == std::string("no") && holo::opts::AccessReferences(true) == std::string("yes"),
          "references only for YouTube items");
    Check(holo::opts::BaseOptions("x", "auto", -600)[0].name == std::string("config"), "first option is config=no");
    Check(holo::opts::SafeHwdec("vaapi") == "auto-copy" && holo::opts::SafeHwdec("d3d11va-copy") == "d3d11va-copy" &&
              holo::opts::SafeHwdec("auto-copy,exec") == "auto-copy",
          "hwdec whitelist");

    const auto raw = holo::opts::YtdlRawOptions("C:\\p,x=y\\deno.exe", "");
    auto has = [&](const holo::opts::Pairs& p, const std::string& k, const std::string& v) {
        for (const auto& [a, b] : p)
            if (a == k && b == v) return true;
        return false;
    };
    for (const char* k : {"ignore-config", "no-plugin-dirs", "no-exec", "no-cache-dir", "no-mark-watched"}) Check(has(raw, k, ""), std::string("raw ") + k);
    Check(has(raw, "use-extractors", "youtube,youtube:tab"), "raw use-extractors");
    Check(has(raw, "js-runtimes", "deno:C:\\p,x=y\\deno.exe"), "a comma or '=' in the deno path stays inside one pair");
    Check(raw.size() == 7, "no subtitle options without a language");
    Check(has(holo::opts::YtdlRawOptions("d", "en"), "sub-langs", "en.*") && has(holo::opts::YtdlRawOptions("d", "en"), "write-subs", ""),
          "subtitle language");
    for (const char* bad : {"en,exec=calc", "en=1", "en.*", "EN", "e", "english", "en-", "en-US-x-toolong1", "en;rm"})
        Check(holo::opts::YtdlRawOptions("d", bad).size() == 7, std::string("subLang refused: ") + bad);
    Check(has(holo::opts::ScriptOpts("C:\\a,b\\yt-dlp.exe"), "ytdl_hook-ytdl_path", "C:\\a,b\\yt-dlp.exe"), "ytdl path pair");

    const std::string f = holo::opts::YtdlFormat(720);
    Check(f == "bv*[height<=720][vcodec!^=av01][dynamic_range=SDR]+ba/b[height<=720]/bv*[height<=720]+ba/wv*+ba/w", "format " + f);
    Check(holo::opts::CapHeight(1080, 720) == 720 && holo::opts::CapHeight(360, 1080) == 360 && holo::opts::CapHeight(10, 1080) == 144,
          "height cap");

    const auto rv = holo::opts::ResolveArgv(L"C:\\y\\yt-dlp.exe", L"C:\\p\\deno.exe", L"https://www.youtube.com/watch?v=jNQXAC9IVRw");
    Check(rv.size() >= 4 && rv[rv.size() - 2] == L"--" && rv.back() == L"https://www.youtube.com/watch?v=jNQXAC9IVRw", "resolve: -- before the url");
    bool ignore = false, noPlug = false, js = false, flat = false;
    for (size_t i = 0; i < rv.size(); ++i) {
        ignore |= rv[i] == L"--ignore-config";
        noPlug |= rv[i] == L"--no-plugin-dirs";
        js |= rv[i] == L"--js-runtimes" && i + 1 < rv.size() && rv[i + 1] == L"deno:C:\\p\\deno.exe";
        flat |= rv[i] == L"--flat-playlist";
    }
    Check(ignore && noPlug && js && flat, "resolve flags");
}

void TestFit() {
    using holo::FitFrame;
    auto eq = [](holo::Dims d, int w, int h) { return d.w == w && d.h == h; };
    Check(eq(FitFrame(960, 540, 1920, 1080, 16.0 / 9), 960, 540), "1080p into the panel");
    Check(eq(FitFrame(422, 238, 1920, 1080, 16.0 / 9), 416, 234), "1080p into the corner (w multiple of 16)");
    Check(eq(FitFrame(1280, 720, 640, 360, 0), 640, 360), "no upscaling past the video");
    Check(eq(FitFrame(960, 540, 1440, 1080, 4.0 / 3), 720, 540), "4:3 pillarbox fit");
    Check(eq(FitFrame(960, 540, 1920, 800, 2.4), 960, 400), "scope fit");
    Check(eq(FitFrame(5000, 5000, 4000, 4000, 1), 720, 720), "huge box clamped to a slot");
    Check(eq(FitFrame(0, 0, 0, 0, 0), 960, 540), "defaults before any size or video");
    Check(eq(FitFrame(16, 16, 1920, 1080, 16.0 / 9), 16, 8), "tiny box");
    for (int w = 16; w <= 1400; w += 37)
        for (int h = 16; h <= 800; h += 41) {
            const auto d = FitFrame(w, h, 1920, 1080, 16.0 / 9);
            Check(d.w % 16 == 0 && d.h % 2 == 0 && d.w <= 1280 && d.h <= 720 && d.w >= 16 && d.h >= 2, "fit invariants");
        }
}

void TestJson() {
    holo::json::Value v;
    Check(holo::json::Parse(R"({"a":[1,2.5e3,-3],"s":"x\u00e9\ud83d\ude00\n","b":true,"n":null,"o":{}})", &v), "json parse");
    Check(v.Get("a") && v.Get("a")->items.size() == 3 && v.Get("a")->items[1].n == 2500, "json numbers");
    Check(v.Str("s") == "x\xC3\xA9\xF0\x9F\x98\x80\n", "json utf-8 and surrogates");
    Check(holo::json::Parse(R"({"d":NaN,"e":Infinity})", &v) && v.Get("d")->type == holo::json::Type::Null, "python NaN as null");
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "\"\\ud800\"", "\"\\x\"", "{\"a\":1}x", "01x", "\"a\nb\""})
        Check(!holo::json::Parse(bad, &v), std::string("json refused: ") + bad);
    std::string deep(200, '[');
    deep += std::string(200, ']');
    Check(!holo::json::Parse(deep, &v), "json depth cap");
}

void TestResolve() {
    pipe::ResolvedEvt r;
    const char* playlist = R"({"_type":"playlist","id":"PLx","title":"My\u0007 list","entries":[
        {"_type":"url","id":"jNQXAC9IVRw","url":"https://www.youtube.com/watch?v=jNQXAC9IVRw","title":"Me at the zoo","duration":19.0},
        {"_type":"url","id":"[deleted]","url":"","title":"[Deleted video]"},
        {"_type":"url","id":"bad","url":"https://youtu.be/dQw4w9WgXcQ","title":"From url","duration":-5},
        "junk"]})";
    Check(holo::resolve::ParseOutput(playlist, &r), "playlist parses");
    Check(r.title == "My list" && r.items.size() == 2, "playlist items (deleted entry skipped), title cleaned");
    Check(r.items.size() == 2 && r.items[0].url == "https://www.youtube.com/watch?v=jNQXAC9IVRw" && r.items[0].duration == 19 &&
              r.items[1].id == "dQw4w9WgXcQ" && r.items[1].duration == 0,
          "item urls rebuilt from ids, bad duration zeroed");
    pipe::ResolvedEvt one;
    Check(holo::resolve::ParseOutput(R"({"id":"jNQXAC9IVRw","title":"Me at the zoo","duration":19,"formats":[{"a":1}]})", &one) &&
              one.items.size() == 1 && one.title == "Me at the zoo",
          "single video");
    pipe::ResolvedEvt none;
    Check(!holo::resolve::ParseOutput("ERROR: nope", &none), "not json");

    std::string big = R"({"_type":"playlist","title":"t","entries":[)";
    for (int i = 0; i < 300; ++i) {
        if (i) big += ',';
        big += R"({"id":"jNQXAC9IVR)" + std::string(1, static_cast<char>('A' + i % 26)) + R"(","title":")" + std::string(500, 'x') + R"(","duration":1})";
    }
    big += "]}";
    pipe::ResolvedEvt b;
    b.rid = 1;
    Check(holo::resolve::ParseOutput(big, &b) && b.items.size() == proto::kMaxResolveItems, "200 items at most");
    holo::resolve::FitLine(&b);
    const std::string line = pipe::Encode(pipe::Message{b});
    Check(line.size() - 1 <= proto::kMaxLine && b.items.size() == proto::kMaxResolveItems, "a full list fits one pipe line");
    pipe::Message back;
    std::string why;
    const bool decoded = pipe::Decode(std::string_view(line).substr(0, line.size() - 1), pipe::Dir::ToCore, &back, &why);
    Check(decoded && std::get<pipe::ResolvedEvt>(back).items.size() == proto::kMaxResolveItems, "the fitted line decodes on the core side: " + why);

    Check(holo::resolve::ErrorCode("ERROR: [youtube] x: Private video. Sign in") == proto::err::kPrivate, "private");
    Check(holo::resolve::ErrorCode("ERROR: [youtube] x: Video unavailable") == proto::err::kPrivate, "unavailable");
    Check(holo::resolve::ErrorCode("ERROR: Unable to download webpage: <urlopen error [Errno 11001] getaddrinfo failed>") == proto::err::kNetwork,
          "network");
    Check(holo::resolve::ErrorCode("ERROR: [youtube] x: Sign in to confirm you're not a bot") == proto::err::kYtdlp, "bot check is a yt-dlp error");
    Check(holo::resolve::ErrorMessage("WARNING: a\nERROR: first\nsome trace\nERROR: last one\r\nTraceback") == "ERROR: last one", "last ERROR line");
    Check(holo::resolve::ErrorMessage("just text\n") == "just text", "plain message");
}

void TestQuote() {
    for (const wchar_t* s : {L"plain", L"with space", L"C:\\path with\\trailing\\", L"quote\"inside", L"back\\\\\"slash", L"", L"tab\there"}) {
        const std::wstring cmd = L"x.exe " + holo::QuoteArg(s);
        int n = 0;
        wchar_t** v = CommandLineToArgvW(cmd.c_str(), &n);
        Check(v && n == 2 && std::wstring(v[1]) == s, "quote round trip: " + holo::Utf8(s));
        LocalFree(v);
    }
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestArgs();
    TestOptions();
    TestFit();
    TestJson();
    TestResolve();
    TestQuote();
    printf("holo_media_logic_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
