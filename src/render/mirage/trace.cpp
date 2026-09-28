// Module "MirageTrace": GL call statistics from the hub's counters, trace modes, frame capture, texture dumper and
// the GPU compatibility report.
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/bus.h"
#include "melange/gltrace.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "melange/testcmd.h"
#include "render/mirage/compat.h"
#include "render/mirage/hub.h"
#include "render/mirage/stages.h"
#include "render/mirage/trace_internal.h"

namespace melange::mirage::trace {
namespace {
using gltrace::FrameStats;

// ---------------------------------------------------------------- worker
std::mutex g_jobMx;
std::condition_variable g_jobCv;
std::deque<std::function<void()>> g_jobs;
bool g_workerStarted = false;

void WorkerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lk(g_jobMx);
            g_jobCv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            LOG_ERROR("[mirage] trace worker: %s", e.what());
        } catch (...) {
            LOG_ERROR("[mirage] trace worker: unknown exception");
        }
    }
}

// ---------------------------------------------------------------- stats
constexpr int kHist = 600, kWindow = 60;
std::mutex g_statsMx;
FrameStats g_hist[kHist];
int g_histN = 0, g_histIdx = 0;
std::vector<uint32_t> g_prev, g_cat, g_acc, g_lastDelta;
std::vector<double> g_perFrame;
int g_winFrames = 0;

struct SceneAcc {
    uint64_t frames = 0, calls = 0, maxCalls = 0;
    std::vector<uint64_t> fn;
};
std::map<std::string, SceneAcc> g_scenes;

std::mutex g_sceneMx;
std::string g_scene = "menu", g_forcedScene;
uint64_t g_lastTurnTick = 0;

std::atomic<int> g_wantMode{-1};
LARGE_INTEGER g_freq{}, g_lastReturn{};
BOOL(WINAPI* g_swapBuffers)(HDC) = nullptr;

BOOL WINAPI TraceSwapBuffers(HDC dc) {
    BOOL r = g_swapBuffers(dc);
    QueryPerformanceCounter(&g_lastReturn);
    return r;
}

hub::Mode ToHub(gltrace::Mode m) {
    return m == gltrace::Mode::Log ? hub::Mode::Log : m == gltrace::Mode::Count ? hub::Mode::Count : hub::Mode::Passthrough;
}
const char* ModeName(gltrace::Mode m) { return m == gltrace::Mode::Log ? "log" : m == gltrace::Mode::Count ? "count" : "off"; }
bool ParseMode(std::string s, gltrace::Mode* m) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (s == "off") *m = gltrace::Mode::Off;
    else if (s == "count") *m = gltrace::Mode::Count;
    else if (s == "log") *m = gltrace::Mode::Log;
    else return false;
    return true;
}

void UpdateScene() {
    std::lock_guard lk(g_sceneMx);
    if (!g_forcedScene.empty()) g_scene = g_forcedScene;
    else if (g_scene == "match" && GetTickCount64() - g_lastTurnTick > 180000) g_scene = "menu";
}

void Sample() {
    int n = hub::Count();
    const volatile uint32_t* cnt = hub::Counters();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::string scene = Scene();
    std::lock_guard lk(g_statsMx);
    size_t old = g_prev.size();
    if (static_cast<size_t>(n) > old) {
        g_prev.resize(n);
        g_cat.resize(n);
        g_acc.resize(n);
        g_lastDelta.resize(n);
        g_perFrame.resize(n);
        for (int i = static_cast<int>(old); i < n; ++i) {
            g_prev[i] = cnt[i];
            g_cat[i] = Categorize(hub::Name(i));
        }
    }
    FrameStats f{};
    f.frame = events::FrameCount() - 1;
    SceneAcc& sc = g_scenes[scene];
    if (sc.fn.size() < static_cast<size_t>(n)) sc.fn.resize(n);
    for (int i = 0; i < n; ++i) {
        uint32_t c = cnt[i], d = c - g_prev[i];
        g_prev[i] = c;
        g_lastDelta[i] = d;
        g_acc[i] += d;
        sc.fn[i] += d;
        if (!d) continue;
        f.calls += d;
        uint32_t cat = g_cat[i];
        hub::Src src = hub::Source(i);
        if (src == hub::Src::CgGLIat || src == hub::Src::CgGLProc) f.cgCalls += d;
        if (cat & kDraw) f.draws += d;
        if (cat & kProgramSwitch) f.programSwitches += d;
        if (cat & kParamFlush) f.paramFlushes += d;
        if (cat & kFboBind) f.fboBinds += d;
        if (cat & kTexBind) f.texBinds += d;
        if (cat & kTexUpload) f.texUploads += d;
        if (cat & kGetError) f.getErrors += d;
    }
    if (g_lastReturn.QuadPart && g_freq.QuadPart)
        f.busyMs = static_cast<double>(now.QuadPart - g_lastReturn.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
    ++sc.frames;
    sc.calls += f.calls;
    sc.maxCalls = std::max<uint64_t>(sc.maxCalls, f.calls);
    if (++g_winFrames >= kWindow) {
        for (int i = 0; i < n; ++i) {
            g_perFrame[i] = static_cast<double>(g_acc[i]) / g_winFrames;
            g_acc[i] = 0;
        }
        g_winFrames = 0;
    }
    g_hist[g_histIdx] = f;
    g_histIdx = (g_histIdx + 1) % kHist;
    if (g_histN < kHist) ++g_histN;
}

void OnFrame() {
    UpdateScene();
    int want = g_wantMode.exchange(-1);
    if (want >= 0 && hub::Installed()) {
        if (CaptureBusy()) g_wantMode = want;
        else hub::SetMode(ToHub(static_cast<gltrace::Mode>(want)));
    }
    if (hub::Installed() && hub::GetMode() != hub::Mode::Passthrough) {
        if (!g_freq.QuadPart) {
            QueryPerformanceFrequency(&g_freq);
            mem::HookIAT("GDI32.dll", "SwapBuffers", reinterpret_cast<void*>(&TraceSwapBuffers),
                         reinterpret_cast<void**>(&g_swapBuffers));
        }
        Sample();
    }
    CaptureOnFrame();
    TexdumpOnFrame();
}

// ---------------------------------------------------------------- verbs
std::string Arg(std::string_view args, int i) {
    size_t p = 0;
    for (int k = 0;; ++k) {
        while (p < args.size() && args[p] == ' ') ++p;
        size_t q = args.find(' ', p);
        if (q == std::string_view::npos) q = args.size();
        if (k == i) return std::string(args.substr(p, q - p));
        if (q >= args.size()) return {};
        p = q;
    }
}

bool VerbMode(std::string_view args, void*) {
    gltrace::Mode m;
    if (!ParseMode(Arg(args, 0), &m)) return false;
    if (!hub::Installed()) {
        LOG_WARN("[mirage] gltrace.mode %s: the GL hub is not installed ([MirageTrace] Mode=off at start); restart needed", ModeName(m));
        return false;
    }
    gltrace::SetMode(m);
    LOG_INFO("[mirage] gltrace mode -> %s at the next frame", ModeName(m));
    return true;
}

bool VerbStats(std::string_view args, void*) {
    std::string a = Arg(args, 0);
    uint32_t n = a.empty() ? 120u : static_cast<uint32_t>(std::max(1, atoi(a.c_str())));
    FrameStats s = gltrace::Average(n);
    gltrace::FnStat top[10];
    size_t k = gltrace::Top(top, 10);
    std::string t;
    for (size_t i = 0; i < k; ++i) {
        char b[96];
        snprintf(b, sizeof b, "%s%s=%.1f", i ? " " : "", top[i].name, top[i].perFrame);
        t += b;
    }
    LOG_INFO("[mirage] gltrace stats over %u frames (scene %s): calls=%u draws=%u cg=%u progSwitches=%u paramFlushes=%u "
             "fboBinds=%u texBinds=%u texUploads=%u getErrors=%u busyMs=%.3f | top: %s",
             n, Scene().c_str(), s.calls, s.draws, s.cgCalls, s.programSwitches, s.paramFlushes, s.fboBinds, s.texBinds,
             s.texUploads, s.getErrors, s.busyMs, t.c_str());
    jlog::Rec("mirage", jlog::Level::Info, "gltrace.stats")
        .Uint("frames", n).Str("scene", Scene()).Uint("calls", s.calls).Uint("draws", s.draws).Uint("cgCalls", s.cgCalls)
        .Uint("programSwitches", s.programSwitches).Uint("paramFlushes", s.paramFlushes).Uint("fboBinds", s.fboBinds)
        .Uint("texBinds", s.texBinds).Uint("texUploads", s.texUploads).Uint("getErrors", s.getErrors)
        .Float("busyMs", s.busyMs).Str("top", t);
    return true;
}

bool VerbCapture(std::string_view args, void*) {
    gltrace::CaptureOptions o;
    for (int i = 0; i < 3; ++i) {
        std::string a = Arg(args, i);
        if (a == "notex") o.textures = false;
        else if (!a.empty() && isdigit(static_cast<unsigned char>(a[0]))) o.frames = static_cast<uint32_t>(atoi(a.c_str()));
    }
    std::string err;
    if (!CaptureRequest(o, &err)) {
        LOG_WARN("[mirage] gltrace.capture refused: %s", err.c_str());
        return false;
    }
    return true;
}

bool VerbTexdump(std::string_view args, void*) {
    std::string a = Arg(args, 0);
    if (a == "off") {
        gltrace::StopTextureDump();
        return true;
    }
    int n = atoi(a.c_str());
    std::string filter = Arg(args, 1);
    return n > 0 && gltrace::StartTextureDump(static_cast<uint32_t>(n), filter.empty() ? nullptr : filter.c_str());
}

bool VerbScene(std::string_view args, void*) {
    std::string a = Arg(args, 0);
    std::lock_guard lk(g_sceneMx);
    g_forcedScene = a == "auto" ? std::string() : a;
    if (!g_forcedScene.empty()) g_scene = g_forcedScene;
    LOG_INFO("[mirage] gltrace scene label -> %s", a.empty() ? "(auto)" : a.c_str());
    return true;
}

bool VerbStatus(std::string_view, void*) {
    std::wstring path;
    std::string err;
    gltrace::CaptureState st = gltrace::CaptureStatus(&path, &err);
    static const char* kNames[] = {"idle", "armed", "recording", "writing", "done", "failed"};
    LOG_INFO("[mirage] gltrace status: mode=%s hub=%d capture=%s path=%s error=%s texturesDumped=%u",
             ModeName(gltrace::GetMode()), hub::Installed(), kNames[static_cast<int>(st)],
             game::Narrow(path).c_str(), err.c_str(), gltrace::TexturesDumped());
    return true;
}

bool VerbReport(std::string_view, void*) {
    int n = hub::Count();
    int exeIat = 0, exeProc = 0, cgIat = 0, cgProc = 0;
    std::string called;
    int nCalled = 0;
    const volatile uint32_t* cnt = hub::Counters();
    for (int i = 0; i < n; ++i) {
        hub::Src s = hub::Source(i);
        (s == hub::Src::ExeIat ? exeIat : s == hub::Src::ExeProc ? exeProc : s == hub::Src::CgGLIat ? cgIat : cgProc)++;
        if ((s == hub::Src::ExeProc || s == hub::Src::CgGLProc) && cnt[i]) {
            called += std::string(nCalled++ ? " " : "") + hub::Name(i) + (s == hub::Src::CgGLProc ? "(cg)" : "");
        }
    }
    std::wstring path = jlog::CurrentSession().dir + L"\\gltrace_report.tsv";
    std::string tsv = "# scene\tfn\tsource\tperFrame\n";
    {
        std::lock_guard lk(g_statsMx);
        for (auto& [scene, sc] : g_scenes) {
            if (!sc.frames) continue;
            double fr = static_cast<double>(sc.frames);
            char b[160];
            snprintf(b, sizeof b, "# scene %s frames=%llu calls/frame=%.1f max=%llu\n", scene.c_str(),
                     static_cast<unsigned long long>(sc.frames), sc.calls / fr, static_cast<unsigned long long>(sc.maxCalls));
            tsv += b;
            std::vector<std::pair<uint64_t, int>> top;
            for (size_t i = 0; i < sc.fn.size(); ++i)
                if (sc.fn[i]) top.push_back({sc.fn[i], static_cast<int>(i)});
            std::sort(top.rbegin(), top.rend());
            for (auto& [c, i] : top) {
                snprintf(b, sizeof b, "%s\t%s\t%s\t%.2f\n", scene.c_str(), hub::Name(i), SrcName(hub::Source(i)), c / fr);
                tsv += b;
            }
        }
    }
    tsv += "# fn\tindex\tname\tsource\ttotal\n";
    for (int i = 0; i < n; ++i) {
        char b[160];
        snprintf(b, sizeof b, "fn\t%d\t%s\t%s\t%u\n", i, hub::Name(i), SrcName(hub::Source(i)), cnt[i]);
        tsv += b;
    }
    bool ok = WriteFileBytes(path, tsv);
    LOG_INFO("[mirage] gltrace report: hub=%d thunks=%d exe imports=%d exe procs=%d cgGL imports=%d cgGL procs=%d "
             "procsHandedOut=%u; written to %s", hub::Installed(), n, exeIat, exeProc, cgIat, cgProc,
             gltrace::ProcsHandedOut(), ok ? "gltrace_report.tsv" : "(write failed)");
    LOG_INFO("[mirage] gltrace procs called (%d): %s", nCalled, called.c_str());
    jlog::Rec("mirage", jlog::Level::Info, "gltrace.report")
        .Bool("hub", hub::Installed()).Int("thunks", n).Int("exeImports", exeIat).Int("exeProcs", exeProc)
        .Int("cgglImports", cgIat).Int("cgglProcs", cgProc).Uint("procsHandedOut", gltrace::ProcsHandedOut())
        .Int("procsCalled", nCalled).Str("called", called);
    return ok;
}

void MenuCapture(void*) {
    std::string err;
    if (!CaptureRequest(gltrace::CaptureOptions{}, &err)) LOG_WARN("[mirage] capture refused: %s", err.c_str());
}

void OnTurn(const bus::MessageView&, void*) {
    std::lock_guard lk(g_sceneMx);
    g_lastTurnTick = GetTickCount64();
    if (g_forcedScene.empty()) g_scene = "match";
}

class MirageTrace final : public Module {
public:
    const char* Name() const override { return "MirageTrace"; }
    const char* Description() const override { return "GL trace, stats panel, frame capture, texture dumper, GPU report"; }
    int Order() const override { return 41; }

    bool Install() override {
        if (!melange::mirage::stages::CoreEnabled(Name())) return false;
        std::string modeText = String("Mode", "count");
        gltrace::Mode mode = gltrace::Mode::Count;
        if (!ParseMode(modeText, &mode)) LOG_WARN("[mirage] [MirageTrace] Mode=%s unknown; using count", modeText.c_str());
        config::EnsureKey(Name(), "RingLog2", "17");
        std::string hotkey = String("CaptureHotkey", "Ctrl+Shift+F9");
        String("CaptureDir", "");
        int texAtStart = Int("TexDumpAtStart", 0);
        std::string texFilter = String("TexDumpFilter", "");

        compat::Install();
        if (mode != gltrace::Mode::Off) {
            if (hub::Require(Name())) {
                hub::SetMode(ToHub(mode));
                compat::Report(compat::Kind::Feature, "gltrace", compat::Status::Loaded, ModeName(mode), "builtin");
            } else {
                compat::Report(compat::Kind::Feature, "gltrace", compat::Status::Failed, "GL hub unavailable", "builtin");
            }
        } else {
            compat::Report(compat::Kind::Feature, "gltrace", compat::Status::Skipped, "[MirageTrace] Mode=off", "builtin");
        }
        TexdumpInstall(texAtStart > 0 ? static_cast<uint32_t>(texAtStart) : 0, texFilter);

        events::Subscribe(events::Event::Frame, [] { OnFrame(); });
        events::Subscribe(events::Event::MatchEnd, [] {
            std::lock_guard lk(g_sceneMx);
            if (g_forcedScene.empty()) g_scene = "menu";
        });
        bus::SubscribeName("GameLogic.Turn.Started", bus::Path::Post, &OnTurn);
        bus::SubscribeName("GameLogic.Turn.Ended", bus::Path::Post, &OnTurn);

        testcmd::Register("gltrace.mode", &VerbMode);
        testcmd::Register("gltrace.stats", &VerbStats);
        testcmd::Register("gltrace.capture", &VerbCapture);
        testcmd::Register("gltrace.texdump", &VerbTexdump);
        testcmd::Register("gltrace.report", &VerbReport);
        testcmd::Register("gltrace.scene", &VerbScene);
        testcmd::Register("gltrace.status", &VerbStatus);

        RegisterPanel();
        overlay::AddMenuItem("Mirage/Capture GL frame", &MenuCapture, nullptr, hotkey.c_str());
        uint8_t dik = 0, mods = 0;
        if (!hotkey.empty() && overlay::ParseHotkey(hotkey.c_str(), &dik, &mods)) overlay::AddHotkey(dik, mods, &MenuCapture, nullptr);
        else if (!hotkey.empty()) LOG_WARN("[mirage] [MirageTrace] CaptureHotkey=%s not understood", hotkey.c_str());

        LOG_INFO("[mirage] trace ready: mode=%s hub=%d", ModeName(mode), hub::Installed());
        return true;
    }

private:
    std::string String(const char* key, const char* def) const {
        config::EnsureKey(Name(), key, def);
        return config::GetString(Name(), key, def);
    }
};
MELANGE_MODULE(MirageTrace);
}  // namespace

void RunAsync(std::function<void()> job) {
    std::lock_guard lk(g_jobMx);
    if (!g_workerStarted) {
        g_workerStarted = true;
        std::thread(&WorkerLoop).detach();
    }
    g_jobs.push_back(std::move(job));
    g_jobCv.notify_one();
}

std::wstring MelangeDocsDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return (out.empty() ? std::wstring(L".") : out) + L"\\Melange";
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) EnsureDir(dir.substr(0, slash));
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        DWORD n = 0, want = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 22));
        if (!WriteFile(f, data.data() + done, want, &n, nullptr) || !n) {
            ok = false;
            break;
        }
        done += n;
    }
    CloseHandle(f);
    return ok;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Scene() {
    std::lock_guard lk(g_sceneMx);
    return g_scene;
}

History GetHistory() {
    History h;
    std::lock_guard lk(g_statsMx);
    int n = std::min(g_histN, 240);
    for (int k = n; k > 0; --k) {
        const FrameStats& f = g_hist[(g_histIdx - k + kHist) % kHist];
        h.calls.push_back(static_cast<float>(f.calls));
        h.draws.push_back(static_cast<float>(f.draws));
        h.busyMs.push_back(static_cast<float>(f.busyMs));
    }
    return h;
}
}  // namespace melange::mirage::trace



namespace melange::gltrace {
namespace t = mirage::trace;

bool Installed() { return mirage::hub::Installed(); }

Mode GetMode() {
    switch (mirage::hub::GetMode()) {
        case mirage::hub::Mode::Log: return Mode::Log;
        case mirage::hub::Mode::Count: return Mode::Count;
        default: return Mode::Off;
    }
}

void SetMode(Mode m) { t::g_wantMode = static_cast<int>(m); }

FrameStats Last() {
    std::lock_guard lk(t::g_statsMx);
    return t::g_histN ? t::g_hist[(t::g_histIdx - 1 + t::kHist) % t::kHist] : FrameStats{};
}

FrameStats Average(uint32_t frames) {
    std::lock_guard lk(t::g_statsMx);
    int n = std::min<int>(static_cast<int>(std::clamp<uint32_t>(frames, 1, t::kHist)), t::g_histN);
    FrameStats a{};
    if (!n) return a;
    uint64_t s[9] = {};
    double busy = 0;
    for (int k = 1; k <= n; ++k) {
        const FrameStats& f = t::g_hist[(t::g_histIdx - k + t::kHist) % t::kHist];
        const uint32_t v[9] = {f.calls, f.draws, f.cgCalls, f.programSwitches, f.paramFlushes, f.fboBinds, f.texBinds,
                               f.texUploads, f.getErrors};
        for (int i = 0; i < 9; ++i) s[i] += v[i];
        busy += f.busyMs;
        if (k == 1) a.frame = f.frame;
    }
    uint32_t* out[9] = {&a.calls, &a.draws, &a.cgCalls, &a.programSwitches, &a.paramFlushes, &a.fboBinds, &a.texBinds,
                        &a.texUploads, &a.getErrors};
    for (int i = 0; i < 9; ++i) *out[i] = static_cast<uint32_t>((s[i] + n / 2) / n);
    a.busyMs = busy / n;
    return a;
}

size_t Top(FnStat* out, size_t max) {
    std::lock_guard lk(t::g_statsMx);
    std::vector<int> idx;
    for (size_t i = 0; i < t::g_perFrame.size(); ++i)
        if (t::g_perFrame[i] > 0 || t::g_lastDelta[i]) idx.push_back(static_cast<int>(i));
    std::sort(idx.begin(), idx.end(), [](int a, int b) { return t::g_perFrame[a] > t::g_perFrame[b]; });
    size_t n = std::min(max, idx.size());
    for (size_t k = 0; k < n; ++k) {
        int i = idx[k];
        out[k] = {mirage::hub::Name(i), t::SrcBit(mirage::hub::Source(i)), t::g_perFrame[i], t::g_lastDelta[i]};
    }
    return n;
}

uint32_t ProcsHandedOut() {
    uint32_t n = 0;
    for (int i = 0; i < mirage::hub::Count(); ++i) {
        auto s = mirage::hub::Source(i);
        if (s == mirage::hub::Src::ExeProc || s == mirage::hub::Src::CgGLProc) ++n;
    }
    return n;
}

bool RequestCapture(const CaptureOptions& opt) { return t::CaptureRequest(opt, nullptr); }
CaptureState CaptureStatus(std::wstring* path, std::string* error) { return t::CaptureState(path, error); }
}  // namespace melange::gltrace
