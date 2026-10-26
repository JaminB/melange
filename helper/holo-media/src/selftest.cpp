// holo-media.exe --selftest [--libmpv <dll>] [--cache <dir>] [--ytdlp <exe>] [--deno <exe>] [--play <video file>]
// Run by media-pack.yml (C6c) and by tests/lowil_harness.cpp at Low integrity. Offline: it plays lavfi test sources
// through the real player path (hardened options, software renderer, frame section, props) and checks frames and
// time-pos; with --ytdlp it proves that planted yt-dlp configs and plugins are ignored (S1); at Low integrity it proves
// the sandbox (S2): whoami /groups from a child shows Low Mandatory Level, a write into Documents fails, the cache is
// writable, deno runs.
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

#include "app.h"
#include "args.h"
#include "frame_size.h"
#include "log.h"
#include "mpv_api.h"
#include "mpv_opts.h"
#include "player.h"
#include "sandbox.h"
#include "util.h"

namespace holo {
namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;
namespace shm = melange::media::shm;

namespace {
int g_pass = 0, g_fail = 0;
void Check(bool ok, const char* what, const std::string& detail = {}) {
    if (ok) ++g_pass;
    else ++g_fail;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
}
void Info(const char* what, const std::string& detail) { printf("info %s: %s\n", what, detail.c_str()); }

bool WriteFileText(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &w, nullptr) && w == text.size();
    CloseHandle(h);
    return ok;
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---------------------------------------------------------------- S2: the sandbox, when running at Low integrity
void SandboxChecks(const std::wstring& cache, const Args& a) {
    const uint32_t rid = sandbox::IntegrityRid();
    Info("integrity", sandbox::IntegrityName(rid));
    if (!WriteFileText(cache + L"\\selftest-probe.txt", "x")) Check(false, "cache folder writable");
    else Check(true, "cache folder writable");
    DeleteFileW((cache + L"\\selftest-probe.txt").c_str());
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) {
        Info("low-integrity checks", "skipped (not running at Low integrity)");
        return;
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const auto who = sandbox::Run({std::wstring(sys) + L"\\whoami.exe", L"/groups"}, cache, 15000, 1 << 20, 1 << 16);
    Check(who.exitCode == 0 && who.out.find("S-1-16-4096") != std::string::npos, "child process runs at Low Mandatory Level (whoami /groups)",
          who.exitCode == 0 ? "" : "whoami exit " + std::to_string(who.exitCode) + " " + who.why);
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        const std::wstring probe = std::wstring(docs) + L"\\holo-media-lowil-probe-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        const DWORD e = GetLastError();
        Check(h == INVALID_HANDLE_VALUE && e == ERROR_ACCESS_DENIED, "a write into Documents is refused", h == INVALID_HANDLE_VALUE ? WinError(e) : "it succeeded");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CoTaskMemFree(docs);
    }
    if (!a.deno.empty()) {
        const auto d = sandbox::Run({a.deno, L"--version"}, cache + L"\\tmp", 30000, 1 << 16, 1 << 16);
        Check(d.exitCode == 0 && d.out.rfind("deno ", 0) == 0, "deno runs at Low integrity", d.out.substr(0, d.out.find('\n')) + d.why);
    }
}

// ---------------------------------------------------------------- S1: planted yt-dlp configs and plugins are ignored
void YtdlpChecks(const std::wstring& cache, const Args& a) {
    const std::wstring dir = cache + L"\\s1";
    sandbox::DeleteTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring exe = dir + L"\\yt-dlp.exe";
    if (!CopyFileW(a.ytdlp.c_str(), exe.c_str(), FALSE)) {
        Check(false, "yt-dlp copy for the planted-config test", WinError(GetLastError()));
        return;
    }
    const std::wstring marker = dir + L"\\plugin-ran.txt";
    std::string markerPy = Utf8(marker);
    for (auto& c : markerPy)
        if (c == '\\') c = '/';
    // Plugins in the portable location, next to the exe: importing one writes the marker.
    for (const wchar_t* d : {L"\\yt-dlp-plugins", L"\\yt-dlp-plugins\\holo", L"\\yt-dlp-plugins\\holo\\yt_dlp_plugins",
                             L"\\yt-dlp-plugins\\holo\\yt_dlp_plugins\\extractor"})
        CreateDirectoryW((dir + d).c_str(), nullptr);
    WriteFileText(dir + L"\\yt-dlp-plugins\\holo\\yt_dlp_plugins\\extractor\\holo_probe.py",
                  "open(r'" + markerPy + "', 'w').write('x')\n"
                  "from yt_dlp.extractor.common import InfoExtractor\n"
                  "class HoloProbeIE(InfoExtractor):\n    _VALID_URL = r'holoprobe:'\n");
    // --list-extractors works offline, after the configs are read and the plugins imported.
    std::vector<std::wstring> hardened = opts::ResolveArgv(exe, a.deno.empty() ? L"C:\\none\\deno.exe" : a.deno, L"x");
    hardened.resize(hardened.size() - 7);  // drop --flat-playlist -J --no-warnings --playlist-end N -- url
    hardened.push_back(L"--list-extractors");

    // Plugins are imported when yt-dlp loads its extractors for a URL (offline here: the probe's own scheme).
    const auto plugControl = sandbox::Run({exe, L"--ignore-config", L"--simulate", L"holoprobe:x"}, dir, 60000, 1 << 20, 1 << 16);
    const bool pluginLive = Exists(marker);
    Info("plugin control run", "exit " + std::to_string(plugControl.exitCode) + (pluginLive ? ", the planted plugin ran" : ", plugin not imported"));
    DeleteFileW(marker.c_str());
    std::vector<std::wstring> plugArgs(hardened.begin(), hardened.end() - 1);
    plugArgs.insert(plugArgs.end(), {L"--simulate", L"--", L"holoprobe:x"});
    const auto plugHard = sandbox::Run(plugArgs, dir, 60000, 1 << 20, 1 << 16);
    if (pluginLive) Check(!Exists(marker), "no planted plugin runs with the whitelist flags", "exit " + std::to_string(plugHard.exitCode));
    else Info("plugin check", "not demonstrable: this yt-dlp did not import the planted plugin even without --no-plugin-dirs");
    DeleteFileW(marker.c_str());

    // An unknown option makes yt-dlp exit 2 when it reads a config file, so "was it read" is observable offline; the
    // --exec line is what an attacker would plant.
    const std::string conf = "--exec \"cmd /c echo pwned\"\n--holo-planted-config-was-loaded\n";
    WriteFileText(dir + L"\\yt-dlp.conf", conf);                    // the portable config, next to the exe
    const std::wstring appdata = cache + L"\\home\\appdata\\yt-dlp";  // APPDATA as the scrubbed environment sets it
    CreateDirectoryW(appdata.c_str(), nullptr);
    WriteFileText(appdata + L"\\config.txt", conf);
    const auto control = sandbox::Run({exe, L"--list-extractors"}, dir, 60000, 1 << 20, 1 << 16);
    Check(control.exitCode == 2 && control.err.find("holo-planted") != std::string::npos, "the planted configs are live (control run)",
          "exit " + std::to_string(control.exitCode) + (control.started ? "" : " (" + control.why + ")"));
    const auto hard = sandbox::Run(hardened, dir, 60000, 1 << 20, 1 << 16);
    Check(hard.exitCode == 0 && hard.err.find("holo-planted") == std::string::npos && hard.out.find("youtube") != std::string::npos,
          "the whitelist flags ignore every planted config", "exit " + std::to_string(hard.exitCode) + " " + hard.err.substr(0, 200));
    Check(!Exists(marker), "nothing planted ran");
    sandbox::DeleteTree(dir);
    sandbox::DeleteTree(appdata);

    // Language codes that are not plain codes never become raw options.
    auto hasSubs = [](const opts::Pairs& p) {
        for (const auto& [k, v] : p)
            if (k == "sub-langs") return true;
        return false;
    };
    Check(!hasSubs(opts::YtdlRawOptions("d", "en,exec=calc")) && !hasSubs(opts::YtdlRawOptions("d", "en=x")) &&
              hasSubs(opts::YtdlRawOptions("d", "pt-BR")),
          "a subLang with a comma or '=' is dropped");
}

#if HOLO_HAVE_MPV
// ---------------------------------------------------------------- the player path, offline
struct Sink {
    std::mutex mu;
    std::vector<pipe::Message> msgs;
};

// --play <file>: a local video file through the same player and section, start to end (a developer check of the real
// decode path; the core never sends a local file).
void PlayFile(Player& p, PlayerEnv& env, Sink& sink, shm::Reader& reader, const std::string& section, const std::wstring& file) {
    LoadSpec s;
    s.gen = 3;
    s.shm = section;
    s.url = Utf8(file);
    s.paused = false;
    s.mute = true;
    s.volume = 0;
    p.Size(1280, 720);
    std::string code, why;
    Check(p.Load(s, &code, &why), "--play: loadfile", why);
    uint64_t frames = 0, bad = 0;
    bool loaded = false, eof = false, varied = false;
    int videoW = 0, videoH = 0;
    double maxPos = -1;
    const ULONGLONG until = GetTickCount64() + 30000;
    while (GetTickCount64() < until && !eof) {
        WaitForSingleObject(env.wake, 20);
        p.Drain();
        p.Tick();
        shm::Frame f;
        const auto rr = reader.Acquire(&f);
        if (rr == shm::ReadResult::New) {
            if (loaded) {  // frames of this item only (the sine item before it has no video)
                ++frames;
                const uint32_t* px = reinterpret_cast<const uint32_t*>(f.data);
                const uint32_t first = px[0] & 0xFFFFFFu;
                for (uint32_t i = 0; i < f.meta.w * f.meta.h && !varied; i += 997) varied = (px[i] & 0xFFFFFFu) != first;
            }
            reader.Release();
        } else if (rr == shm::ReadResult::Bad) {
            ++bad;
        }
        std::vector<pipe::Message> got;
        {
            std::lock_guard lk(sink.mu);
            got.swap(sink.msgs);
        }
        for (auto& m : got) {
            if (auto* e = std::get_if<pipe::EventEvt>(&m)) {
                loaded = loaded || (e->name == proto::event::kFileLoaded && e->gen == 3);
                eof = eof || (e->name == proto::event::kEndFile && e->reason == "eof" && e->gen == 3);
            } else if (auto* pr = std::get_if<pipe::PropEvt>(&m)) {
                if (pr->gen != 3) continue;
                if (pr->name == proto::prop::kTimePos && pr->kind == pipe::PropEvt::Kind::Number) maxPos = (std::max)(maxPos, pr->number);
                if (pr->name == proto::prop::kVideoParams && pr->kind == pipe::PropEvt::Kind::Video) {
                    videoW = pr->video.w;
                    videoH = pr->video.h;
                }
            } else if (auto* er = std::get_if<pipe::ErrorEvt>(&m)) {
                Check(false, "--play: no player error", er->code + ": " + er->message);
            }
        }
    }
    Check(loaded, "--play: file-loaded event");
    Check(eof, "--play: end-file eof event");
    Check(frames >= 30 && bad == 0, "--play: frames published into the section", std::to_string(frames) + " read, " + std::to_string(bad) + " bad");
    Check(varied, "--play: frames carry the picture (not blank)");
    Check(maxPos >= 2.0, "--play: time-pos advances", std::to_string(maxPos));
    Info("--play video-params", std::to_string(videoW) + "x" + std::to_string(videoH));
}

void PlayerChecks(const std::wstring& cache, const Args& a) {
    std::string why;
    if (!mpv::Load(a.libmpv, &why)) {
        Check(false, "libmpv loads", why);
        return;
    }
    Check(true, "libmpv loads");
    std::string mv, fv;
    App::ProbeVersions(Utf8(cache + L"\\mpvhome"), &mv, &fv);
    Check(!mv.empty(), "mpv version", mv + " / " + fv);

    // The section the core would create: same layout, a name of the core's pattern.
    const std::wstring name = L"Local\\melange-holo-" + std::to_wstring(GetCurrentProcessId()) + L"-5e1f7e57-1";
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(shm::SectionBytes()), name.c_str());
    if (!sec) {
        Check(false, "frame section", WinError(GetLastError()));
        return;
    }
    void* view = MapViewOfFile(sec, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    shm::InitHeader(view);
    shm::Reader reader(view);

    Sink sink;
    PlayerEnv env;
    env.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    env.mpvHome = Utf8(cache + L"\\mpvhome");
    int roots = 0;
    const std::wstring ca = cache + L"\\home\\roots.pem";
    const bool rootsOk = sandbox::WriteRootBundle(ca, &roots) && roots > 0;
    Check(rootsOk, "the Windows trusted roots export for mpv's TLS", std::to_string(roots) + " roots");
    env.caFile = Utf8(ca);
    env.ytdlp = Utf8(a.ytdlp);
    env.deno = Utf8(a.deno);
    env.send = [&](const pipe::Message& m, bool) {
        std::lock_guard lk(sink.mu);
        sink.msgs.push_back(m);
    };
    auto p = Player::Create(1, env, &why);
    Check(p != nullptr, "player with the hardened options", why);
    if (!p) return;

    LoadSpec s;
    s.gen = 1;
    s.shm = Utf8(name);
    s.url = "av://lavfi:testsrc=duration=3:size=640x360:rate=30";
    s.paused = false;
    s.mute = true;
    s.volume = 0;
    std::string code;
    const bool queued = p->Load(s, &code, &why);
    Check(queued, "loadfile", why);

    uint64_t frames = 0, bad = 0, resized = 0;
    bool varied = false, loaded = false, eof = false, sizeSent = false;
    double lastPos = -1, maxPos = -1;
    int posProps = 0, posBackwards = 0, videoW = 0, videoH = 0;
    const ULONGLONG until = GetTickCount64() + 20000;
    while (GetTickCount64() < until && !eof) {
        WaitForSingleObject(env.wake, 20);
        p->Drain();
        p->Tick();
        shm::Frame f;
        const auto rr = reader.Acquire(&f);
        if (rr == shm::ReadResult::New) {
            ++frames;
            if (f.meta.w == 320) ++resized;
            const uint32_t* px = reinterpret_cast<const uint32_t*>(f.data);
            const uint32_t first = px[0] & 0xFFFFFFu;
            for (uint32_t i = 0; i < f.meta.w * f.meta.h && !varied; i += 997) varied = (px[i] & 0xFFFFFFu) != first;
            reader.Release();
        } else if (rr == shm::ReadResult::Bad) {
            ++bad;
        }
        std::vector<pipe::Message> got;
        {
            std::lock_guard lk(sink.mu);
            got.swap(sink.msgs);
        }
        for (auto& m : got) {
            if (auto* e = std::get_if<pipe::EventEvt>(&m)) {
                loaded = loaded || (e->name == proto::event::kFileLoaded && e->gen == 1);
                eof = eof || (e->name == proto::event::kEndFile && e->reason == "eof" && e->gen == 1);
            } else if (auto* pr = std::get_if<pipe::PropEvt>(&m)) {
                // every line must survive the core's decoder
                pipe::Message back;
                std::string dw;
                const std::string line = pipe::Encode(m);
                if (!pipe::Decode(std::string_view(line).substr(0, line.size() - 1), pipe::Dir::ToCore, &back, &dw)) Check(false, "prop decodes", dw);
                if (pr->name == proto::prop::kTimePos && pr->kind == pipe::PropEvt::Kind::Number) {
                    ++posProps;
                    if (pr->number + 1e-6 < lastPos) ++posBackwards;
                    lastPos = pr->number;
                    if (pr->number > maxPos) maxPos = pr->number;
                    if (pr->qpcMs <= 0) Check(false, "time-pos carries qpc");
                } else if (pr->name == proto::prop::kVideoParams && pr->kind == pipe::PropEvt::Kind::Video) {
                    videoW = pr->video.w;
                    videoH = pr->video.h;
                }
            } else if (auto* er = std::get_if<pipe::ErrorEvt>(&m)) {
                Check(false, "no player error", er->code + ": " + er->message);
            }
        }
        if (!sizeSent && frames >= 20) {
            p->Size(320, 180);  // the corner size: later frames come at 320x180
            sizeSent = true;
        }
    }
    Check(loaded, "file-loaded event");
    Check(eof, "end-file eof event");
    Check(frames >= 30, "frames published into the section", std::to_string(frames) + " read, " + std::to_string(p->FramesPublished()) + " published");
    Check(bad == 0, "every frame passes the core's checks", std::to_string(bad) + " bad");
    Check(varied, "frames carry the picture (not blank)");
    Check(resized > 0, "a size hint changes the frame size", std::to_string(resized) + " frames at 320x180");
    Check(videoW == 640 && videoH == 360, "video-params", std::to_string(videoW) + "x" + std::to_string(videoH));
    Check(posProps >= 5 && posProps <= 60 && posBackwards == 0 && maxPos >= 2.0, "time-pos throttled and advancing",
          std::to_string(posProps) + " values, max " + std::to_string(maxPos));
    const Dims d = FitFrame(0, 0, 640, 360, 0);
    Check(d.w == 640 && d.h == 360, "frame fit at the video's own size");

    // Audio: the output mpv opens (wasapi on a desktop; CI runners may have none).
    s.gen = 2;
    s.url = "av://lavfi:sine=frequency=440:duration=1";
    p->Load(s, &code, &why);
    std::string ao;
    const ULONGLONG until2 = GetTickCount64() + 10000;
    bool loaded2 = false;
    while (GetTickCount64() < until2 && !loaded2) {
        WaitForSingleObject(env.wake, 20);
        p->Drain();
        std::lock_guard lk(sink.mu);
        for (auto& m : sink.msgs)
            if (auto* e = std::get_if<pipe::EventEvt>(&m)) loaded2 = loaded2 || (e->name == proto::event::kFileLoaded && e->gen == 2);
        sink.msgs.clear();
    }
    Sleep(300);
    p->Drain();
    ao = p->GetString("current-ao");
    Info("audio output", ao.empty() ? "none" : ao);
    if (!a.play.empty()) PlayFile(*p, env, sink, reader, Utf8(name), a.play);
    Player::Close(std::move(p));
    Check(Player::WaitTeardowns(5000), "player teardown");
    UnmapViewOfFile(view);
    CloseHandle(sec);
}
#endif
}  // namespace

int RunSelftest(const Args& a) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring cache = a.cache;
    if (cache.empty()) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        cache = std::wstring(tmp) + L"holo-media-selftest-" + std::to_wstring(GetCurrentProcessId());
    }
    std::string why;
    if (!sandbox::PrepareCache(cache, &why)) {
        printf("FAIL cache: %s\n", why.c_str());
        return 1;
    }
    log::Open(cache + L"\\holo-media.log");
    sandbox::ScrubEnvironment(cache);
    sandbox::JoinOwnJob();
    printf("holo-media %s selftest, protocol %d\n", HOLO_MEDIA_VERSION, proto::kProto);
    SandboxChecks(cache, a);
    if (!a.ytdlp.empty()) YtdlpChecks(cache, a);
    else Info("yt-dlp checks", "skipped (no --ytdlp)");
    sandbox::ApplyMitigations();
#if HOLO_HAVE_MPV
    if (!a.libmpv.empty()) PlayerChecks(cache, a);
    else Check(false, "player checks need --libmpv");
#else
    Info("player checks", "skipped (built without libmpv headers)");
#endif
    printf("holo-media selftest: %d passed, %d failed\n", g_pass, g_fail);
    if (a.cache.empty()) sandbox::DeleteTree(cache);
    return g_fail ? 1 : 0;
}
}  // namespace holo
