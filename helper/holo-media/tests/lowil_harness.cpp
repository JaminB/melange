// holo_media_lowil_test: drives holo-media.exe the way melange.asi will, as a fake core, with the helper at Low
// integrity (Critique S2). Local only (it plays through the speakers' device, muted):
//   holo_media_lowil_test.exe --helper <holo-media.exe> --libmpv <dll> --ytdlp <exe> --deno <exe> [--online]
// 1. `--selftest` at Low: whoami /groups from a child shows Low Mandatory Level, a write into Documents fails, deno
//    runs, the planted yt-dlp configs and plugins are ignored, lavfi plays into a section, WASAPI opens.
// 2. The pipe at Low: the core-side pipe and section carry a Low mandatory label (what C4b creates), the helper starts
//    suspended in a kill-on-close job, connects, says hello, survives garbage, an oversized line and unknown messages,
//    answers ping, refuses an unknown player, a missing section and torrents without an engine, sweeps a dead sibling
//    cache folder but not a live one, and exits on quit.
// 3. --online: resolve and play a short YouTube video at 360p through yt-dlp + deno at Low integrity: frames, time-pos,
//    audio-pts, size, seek, pause, close.
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/pipe_codec.h"
#include "media/shm_logic.h"
#include "sandbox.h"
#include "util.h"

namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;
namespace shm = melange::media::shm;
using holo::Utf8;
using holo::WinError;

namespace {
int g_pass = 0, g_fail = 0;
void Check(bool ok, const std::string& what, const std::string& detail = {}) {
    if (ok) ++g_pass;
    else ++g_fail;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
}

std::wstring UserSid() {
    HANDLE t;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t);
    DWORD n = 0;
    GetTokenInformation(t, TokenUser, nullptr, 0, &n);
    std::vector<uint8_t> b(n);
    GetTokenInformation(t, TokenUser, b.data(), n, &n);
    CloseHandle(t);
    wchar_t* s = nullptr;
    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid, &s);
    std::wstring r = s;
    LocalFree(s);
    return r;
}

// Current user and SYSTEM only, with a Low "no write up" label: what the core gives the pipe and the sections.
struct LowSa {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    LowSa() {
        const std::wstring sddl = L"D:P(A;;GA;;;" + UserSid() + L")(A;;GA;;;SY)S:(ML;;NW;;;LW)";
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
    }
    ~LowSa() { LocalFree(sa.lpSecurityDescriptor); }
};

HANDLE LowToken() {
    HANDLE t = nullptr, low = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &t);
    DuplicateTokenEx(t, 0, nullptr, SecurityImpersonation, TokenPrimary, &low);
    CloseHandle(t);
    PSID sid = nullptr;
    ConvertStringSidToSidW(L"S-1-16-4096", &sid);
    TOKEN_MANDATORY_LABEL tml{};
    tml.Label.Attributes = SE_GROUP_INTEGRITY;
    tml.Label.Sid = sid;
    if (!SetTokenInformation(low, TokenIntegrityLevel, &tml, sizeof tml + GetLengthSid(sid))) printf("SetTokenInformation: %s\n", WinError(GetLastError()).c_str());
    LocalFree(sid);
    return low;
}

struct Child {
    PROCESS_INFORMATION pi{};
    HANDLE job = nullptr, out = nullptr;
};
bool g_medium = false;   // --medium: the [Media] HelperLowIntegrity=0 fallback, for comparison
std::wstring g_keepLog;  // --keep-log <file>: a copy of the helper's log (the cache folder is deleted)

// Starts `argv` at Low integrity, suspended, in a kill-on-close job, then resumes it (the core's launch order).
bool StartLow(const std::vector<std::wstring>& argv, bool captureStdout, Child* c) {
    HANDLE tok = nullptr;
    if (g_medium) OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &tok);
    else tok = LowToken();
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    HANDLE outW = nullptr;
    std::vector<uint8_t> attrBuf;
    if (captureStdout) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        CreatePipe(&c->out, &outW, &sa, 0);
        SetHandleInformation(c->out, HANDLE_FLAG_INHERIT, 0);
        SIZE_T n = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &n);
        attrBuf.resize(n);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &n);
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &outW, sizeof outW, nullptr, nullptr);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = outW;
        si.StartupInfo.hStdError = outW;
    }
    std::wstring cmd = holo::CommandLine(argv);
    const BOOL ok = CreateProcessAsUserW(tok, argv[0].c_str(), cmd.data(), nullptr, nullptr, captureStdout,
                                         CREATE_SUSPENDED | CREATE_NO_WINDOW | (captureStdout ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                         nullptr, &si.StartupInfo, &c->pi);
    const DWORD e = GetLastError();
    if (outW) CloseHandle(outW);
    CloseHandle(tok);
    if (!ok) {
        printf("CreateProcessAsUser: %s\n", WinError(e).c_str());
        return false;
    }
    c->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &li, sizeof li);
    Check(AssignProcessToJobObject(c->job, c->pi.hProcess) != 0, "helper assigned to the job before it runs");
    ResumeThread(c->pi.hThread);
    return true;
}

// A just-exited helper's folder can stay busy for a moment (the virus scanner, the indexer): retry the removal.
void RemoveTree(const std::wstring& dir) {
    for (int i = 0; i < 30 && !holo::sandbox::DeleteTree(dir) && GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) Sleep(100);
}

std::wstring CacheRoot() {
    PWSTR p = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &p);
    std::wstring r = std::wstring(p) + L"\\Melange";
    CoTaskMemFree(p);
    return r;
}
std::string Hex64() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    char b[20];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(t.QuadPart * 0x9E3779B97F4A7C15ull));
    return b;
}

// ---------------------------------------------------------------- 1. selftest at Low
void SelftestAtLow(const std::wstring& helper, const std::wstring& libmpv, const std::wstring& ytdlp, const std::wstring& deno,
                   const std::wstring& cache) {
    Child c;
    if (!StartLow({helper, L"--selftest", L"--libmpv", libmpv, L"--ytdlp", ytdlp, L"--deno", deno, L"--cache", cache}, true, &c)) {
        Check(false, "selftest started at Low");
        return;
    }
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(c.out, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    WaitForSingleObject(c.pi.hProcess, 120000);
    DWORD code = 99;
    GetExitCodeProcess(c.pi.hProcess, &code);
    printf("---- holo-media --selftest at Low integrity\n%s----\n", out.c_str());
    Check(code == 0, "--selftest passes at Low integrity", "exit " + std::to_string(code));
    Check(out.find("info integrity: low") != std::string::npos, "the selftest ran at Low");
    Check(out.find("ok   child process runs at Low Mandatory Level") != std::string::npos, "whoami /groups from a helper child: Low Mandatory Level");
    Check(out.find("ok   a write into Documents is refused") != std::string::npos, "a write into Documents fails");
    Check(out.find("info audio output: wasapi") != std::string::npos, "WASAPI audio opens at Low integrity");
    Check(out.find("ok   deno runs at Low integrity") != std::string::npos, "deno runs at Low integrity");
    CloseHandle(c.out);
    CloseHandle(c.pi.hThread);
    CloseHandle(c.pi.hProcess);
    CloseHandle(c.job);
}

// ---------------------------------------------------------------- 2./3. the pipe, as the core
class FakeCore {
public:
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::mutex mu;
    std::deque<pipe::Message> in;
    std::atomic<bool> closed{false};
    std::thread reader;
    uint64_t undecodable = 0;

    void Start() {
        reader = std::thread([this] {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            pipe::LineSplitter split;
            char buf[65536];
            for (;;) {
                DWORD got = 0;
                ResetEvent(ov.hEvent);
                if (!ReadFile(pipe, buf, sizeof buf, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
                if (!GetOverlappedResult(pipe, &ov, &got, TRUE)) break;
                split.Feed(buf, got, [&](std::string_view line) {
                    pipe::Message m;
                    std::string why;
                    if (!pipe::Decode(line, pipe::Dir::ToCore, &m, &why)) {
                        ++undecodable;
                        printf("undecodable helper line: %s\n", why.c_str());
                        return;
                    }
                    std::lock_guard lk(mu);
                    in.push_back(std::move(m));
                });
            }
            closed = true;
            CloseHandle(ov.hEvent);
        });
    }
    void Send(const std::string& line) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD put = 0;
        if (!WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), nullptr, &ov) && GetLastError() == ERROR_IO_PENDING)
            GetOverlappedResult(pipe, &ov, &put, TRUE);
        CloseHandle(ov.hEvent);
    }
    void Send(const pipe::Message& m) { Send(pipe::Encode(m)); }
    // Waits for the first message `pred` accepts; earlier messages are passed to `seen` (may be null).
    template <class P>
    bool WaitFor(uint32_t ms, P&& pred, const std::function<void(const pipe::Message&)>& seen = nullptr) {
        const ULONGLONG until = GetTickCount64() + ms;
        while (GetTickCount64() < until) {
            std::deque<pipe::Message> got;
            {
                std::lock_guard lk(mu);
                got.swap(in);
            }
            for (size_t i = 0; i < got.size(); ++i) {
                if (seen) seen(got[i]);
                if (pred(got[i])) {
                    std::lock_guard lk(mu);
                    in.insert(in.begin(), got.begin() + static_cast<long long>(i) + 1, got.end());
                    return true;
                }
            }
            Sleep(10);
        }
        return false;
    }
};

template <class T>
const T* As(const pipe::Message& m) {
    return std::get_if<T>(&m);
}

void PipeAtLow(const std::wstring& helper, const std::wstring& libmpv, const std::wstring& ytdlp, const std::wstring& deno,
               const std::wstring& cacheParent, bool online) {
    LowSa sa;
    const std::string rnd = Hex64();
    const std::wstring name = L"\\\\.\\pipe\\melange-holo-" + std::to_wstring(GetCurrentProcessId()) + L"-" + holo::Wide(rnd);
    FakeCore core;
    core.pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16, 1 << 16, 0, &sa.sa);
    if (core.pipe == INVALID_HANDLE_VALUE) {
        Check(false, "pipe server", WinError(GetLastError()));
        return;
    }
    const std::wstring cache = cacheParent + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + holo::Wide(rnd.substr(0, 8));
    CreateDirectoryW(cache.c_str(), nullptr);
    // Sweep fixtures: a dead instance (its lock free) and a live one (lock held without delete sharing).
    const std::wstring dead = cacheParent + L"\\1-dead", live = cacheParent + L"\\2-a11e";
    CreateDirectoryW(dead.c_str(), nullptr);
    CreateDirectoryW((dead + L"\\bt").c_str(), nullptr);
    CloseHandle(CreateFileW((dead + L"\\lock").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
    CloseHandle(CreateFileW((dead + L"\\bt\\piece").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
    CreateDirectoryW(live.c_str(), nullptr);
    HANDLE liveLock = CreateFileW((live + L"\\lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);

    Child c;
    if (!StartLow({helper, L"--pipe", name, L"--parent", std::to_wstring(GetCurrentProcessId()), L"--cache", cache, L"--libmpv", libmpv,
                   L"--ytdlp", ytdlp, L"--deno", deno, L"--sweep"},
                  false, &c)) {
        Check(false, "helper started at Low");
        return;
    }
    Check(holo::sandbox::IntegrityRid([&] {
              HANDLE t = nullptr;
              OpenProcessToken(c.pi.hProcess, TOKEN_QUERY, &t);
              return t;
          }()) == static_cast<uint32_t>(g_medium ? SECURITY_MANDATORY_MEDIUM_RID : SECURITY_MANDATORY_LOW_RID),
          g_medium ? "the helper process runs at Medium (--medium)" : "the helper process runs at Low integrity");
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ConnectNamedPipe(core.pipe, &ov)) {
        const DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            const HANDLE w[2] = {ov.hEvent, c.pi.hProcess};
            if (WaitForMultipleObjects(2, w, FALSE, 15000) != WAIT_OBJECT_0) {
                Check(false, "the helper connects to the pipe");
                TerminateJobObject(c.job, 1);
                return;
            }
        } else if (e != ERROR_PIPE_CONNECTED) {
            Check(false, "the helper connects to the pipe", WinError(e));
            return;
        }
    }
    CloseHandle(ov.hEvent);
    ULONG clientPid = 0;
    GetNamedPipeClientProcessId(core.pipe, &clientPid);
    Check(clientPid == c.pi.dwProcessId, "the pipe client is the helper's own pid");
    core.Start();

    pipe::HelloEvt hello;
    Check(core.WaitFor(15000, [&](const pipe::Message& m) {
        if (auto* h = As<pipe::HelloEvt>(m)) hello = *h;
        return As<pipe::HelloEvt>(m) != nullptr;
    }), "hello from the helper");
    Check(hello.proto == proto::kProto && !hello.mpv.empty(), "hello carries the protocol and the mpv version", hello.mpv + " / " + hello.ffmpeg);
    pipe::HelloCmd hc;
    hc.audioDelayMs = 0;
    core.Send(pipe::Message{hc});

    // Hostile input from "the core" side: none of it may stop the helper.
    core.Send("not json at all\n");
    core.Send(std::string(70000, 'x') + "\n");
    core.Send("{\"t\":\"evil\",\"cmd\":\"calc\"}\n");
    core.Send("{\"t\":\"open\",\"id\":9,\"gen\":1,\"shm\":\"Local\\\\melange-holo-1-2-3\",\"url\":\"https://evil.example/x\"}\n");
    core.Send(pipe::Message{pipe::PingCmd{}});
    Check(core.WaitFor(3000, [](const pipe::Message& m) { return As<pipe::PongEvt>(m) != nullptr; }), "pong after garbage, an oversized line and unknown messages");

    pipe::SetCmd set;
    set.id = 77;
    set.prop = proto::setprop::kPause;
    set.isBool = true;
    core.Send(pipe::Message{set});
    Check(core.WaitFor(3000, [](const pipe::Message& m) {
        auto* e = As<pipe::ErrorEvt>(m);
        return e && e->id == 77 && e->code == proto::err::kProto;
    }), "a command for an unknown player answers error proto");

    pipe::BtAddCmd ba;
    ba.ih = "0123456789abcdef0123456789abcdef01234567";
    ba.magnet = "magnet:?xt=urn:btih:" + ba.ih;
    core.Send(pipe::Message{ba});
    bool btAnswered = core.WaitFor(3000, [&](const pipe::Message& m) {
        auto* e = As<pipe::ErrorEvt>(m);
        return e && e->ih == ba.ih && e->code == proto::err::kTorrent;
    });
    printf("info bt.add: %s\n", btAnswered ? "error torrent (no engine in this build)" : "accepted (torrent engine present)");
    if (btAnswered) Check(true, "bt.* without an engine answers error torrent");
    else core.Send(pipe::Message{pipe::BtRemoveCmd{ba.ih}});

    pipe::OpenCmd miss;
    miss.id = 5;
    miss.gen = 1;
    miss.shm = "Local\\melange-holo-" + std::to_string(GetCurrentProcessId()) + "-deadbeef-5";
    miss.url = "https://www.youtube.com/watch?v=jNQXAC9IVRw";
    core.Send(pipe::Message{miss});
    Check(core.WaitFor(10000, [](const pipe::Message& m) {
        auto* e = As<pipe::ErrorEvt>(m);
        return e && e->id == 5 && e->code == proto::err::kShm;
    }), "an open with a missing frame section answers error shm");
    core.Send(pipe::Message{pipe::CloseCmd{5}});

    Check(GetFileAttributesW(dead.c_str()) == INVALID_FILE_ATTRIBUTES, "--sweep removed the dead instance folder");
    Check(GetFileAttributesW(live.c_str()) != INVALID_FILE_ATTRIBUTES, "--sweep kept the live instance folder");
    CloseHandle(liveLock);
    holo::sandbox::DeleteTree(live);

    if (online) {
        // ---- resolve
        pipe::ResolveCmd rc;
        rc.rid = 1;
        rc.url = "https://www.youtube.com/watch?v=jNQXAC9IVRw";
        core.Send(pipe::Message{rc});
        pipe::ResolvedEvt res;
        const ULONGLONG t0 = GetTickCount64();
        const bool got = core.WaitFor(35000, [&](const pipe::Message& m) {
            if (auto* r = As<pipe::ResolvedEvt>(m); r && r->rid == 1) {
                res = *r;
                return true;
            }
            return false;
        });
        Check(got && res.error.empty() && res.items.size() == 1 && res.items[0].id == "jNQXAC9IVRw", "resolve through yt-dlp + deno at Low",
              got ? (res.error.empty() ? res.items.empty() ? "no items" : "\"" + res.items[0].title + "\", " + std::to_string(res.items[0].duration) + " s, " +
                                                                             std::to_string(GetTickCount64() - t0) + " ms"
                                       : res.error + ": " + res.message)
                  : "no answer");

        // ---- play
        const std::wstring secName = L"Local\\melange-holo-" + std::to_wstring(GetCurrentProcessId()) + L"-" + holo::Wide(rnd.substr(0, 8)) + L"-2";
        HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa.sa, PAGE_READWRITE, 0, static_cast<DWORD>(shm::SectionBytes()), secName.c_str());
        void* view = sec ? MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0) : nullptr;
        Check(view != nullptr, "Low-labelled frame section");
        if (!view) return;
        shm::InitHeader(view);
        shm::Reader reader(view);
        pipe::OpenCmd op;
        op.id = 2;
        op.gen = 1;
        op.shm = Utf8(secName);
        op.url = rc.url;
        op.paused = false;
        op.mute = true;
        op.volume = 0;
        op.height = 360;
        core.Send(pipe::Message{op});
        uint64_t frames = 0, bad = 0, smallFrames = 0;
        int posCount = 0, ptsCount = 0;
        double maxPos = -1, lastPos = -1;
        bool loaded = false, sized = false, sought = false, paused = false;
        bool sawPauseTrue = false;
        std::string err;
        auto watch = [&](const pipe::Message& m) {
            if (auto* p = As<pipe::PropEvt>(m); p && p->id == 2 && p->gen == 1) {
                if (p->name == proto::prop::kTimePos && p->kind == pipe::PropEvt::Kind::Number) {
                    ++posCount;
                    lastPos = p->number;
                    if (p->number > maxPos) maxPos = p->number;
                }
                if (p->name == proto::prop::kAudioPts && p->kind == pipe::PropEvt::Kind::Number) ++ptsCount;
                if (p->name == proto::prop::kPause && p->kind == pipe::PropEvt::Kind::Bool && p->boolean && paused) sawPauseTrue = true;
            } else if (auto* e = As<pipe::EventEvt>(m); e && e->id == 2 && e->name == proto::event::kFileLoaded) {
                loaded = true;
            } else if (auto* er = As<pipe::ErrorEvt>(m); er && er->id == 2) {
                err = er->code + ": " + er->message;
            }
        };
        const ULONGLONG until = GetTickCount64() + 90000;
        ULONGLONG pausedAt = 0;
        while (GetTickCount64() < until && err.empty()) {
            core.WaitFor(20, [](const pipe::Message&) { return false; }, watch);
            shm::Frame f;
            const auto r = reader.Acquire(&f);
            if (r == shm::ReadResult::New) {
                ++frames;
                if (f.meta.w <= 320) ++smallFrames;
                reader.Release();
            } else if (r == shm::ReadResult::Bad) {
                ++bad;
            }
            if (!sized && frames >= 30) {
                core.Send(pipe::Message{pipe::SizeCmd{2, 320, 180}});
                sized = true;
            }
            if (sized && !sought && smallFrames >= 10) {
                pipe::SeekCmd sk;
                sk.id = 2;
                sk.t = 10;
                core.Send(pipe::Message{sk});
                sought = true;
            }
            if (sought && !paused && lastPos >= 11) {
                pipe::SetCmd ps;
                ps.id = 2;
                ps.prop = proto::setprop::kPause;
                ps.isBool = true;
                ps.boolean = true;
                core.Send(pipe::Message{ps});
                paused = true;
                pausedAt = GetTickCount64();
            }
            if (paused && GetTickCount64() - pausedAt > 1500) break;
        }
        Check(err.empty(), "no player error", err);
        Check(loaded, "file-loaded for the YouTube item");
        Check(frames >= 30 && bad == 0, "YouTube frames in the section at Low", std::to_string(frames) + " frames, " + std::to_string(bad) + " bad");
        Check(smallFrames >= 10, "size 320x180 applied", std::to_string(smallFrames) + " small frames");
        Check(posCount >= 5 && maxPos >= 11, "time-pos advances and the seek landed", std::to_string(posCount) + " values, max " + std::to_string(maxPos));
        Check(ptsCount >= 5, "audio-pts reported (audio decoded and output)", std::to_string(ptsCount) + " values");
        Check(sawPauseTrue, "pause applied");
        core.Send(pipe::Message{pipe::CloseCmd{2}});
        Sleep(500);
        UnmapViewOfFile(view);
        CloseHandle(sec);
    }

    core.Send(pipe::Message{pipe::QuitCmd{}});
    const DWORD w = WaitForSingleObject(c.pi.hProcess, 15000);
    DWORD code = 99;
    GetExitCodeProcess(c.pi.hProcess, &code);
    Check(w == WAIT_OBJECT_0 && code == 0, "quit ends the helper", "exit " + std::to_string(code));
    Check(core.undecodable == 0, "every helper line decodes in the core's direction");
    if (core.reader.joinable()) {
        CancelIoEx(core.pipe, nullptr);
        core.reader.join();
    }
    // The helper's log, for the record: which audio output and decoder mpv used.
    HANDLE lf = CreateFileW((cache + L"\\holo-media.log").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lf != INVALID_HANDLE_VALUE) {
        std::string log(1 << 20, '\0');
        DWORD got = 0;
        ReadFile(lf, log.data(), static_cast<DWORD>(log.size()), &got, nullptr);
        log.resize(got);
        CloseHandle(lf);
        printf("---- helper log (%u bytes)\n%s----\n", got, log.size() > 6000 ? log.substr(log.size() - 6000).c_str() : log.c_str());
        if (online) Check(log.find("(ao wasapi") != std::string::npos, "the YouTube item played through WASAPI");
    }
    CloseHandle(c.pi.hThread);
    CloseHandle(c.pi.hProcess);
    CloseHandle(c.job);
    CloseHandle(core.pipe);
    if (!g_keepLog.empty()) CopyFileW((cache + L"\\holo-media.log").c_str(), g_keepLog.c_str(), FALSE);
    RemoveTree(cache);
}

// The core goes away without a quit (a crash, or the watchdog closing the pipe): the helper ends by itself, and it
// does not end merely because no ping comes (8 s of silence first).
void PipeClosed(const std::wstring& helper, const std::wstring& libmpv, const std::wstring& ytdlp, const std::wstring& deno,
                const std::wstring& cacheParent) {
    LowSa sa;
    const std::string rnd = Hex64();
    const std::wstring name = L"\\\\.\\pipe\\melange-holo-" + std::to_wstring(GetCurrentProcessId()) + L"-" + holo::Wide(rnd);
    HANDLE p = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16, 1 << 16, 0, &sa.sa);
    const std::wstring cache = cacheParent + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + holo::Wide(rnd.substr(8, 8));
    CreateDirectoryW(cache.c_str(), nullptr);
    Child c;
    if (p == INVALID_HANDLE_VALUE ||
        !StartLow({helper, L"--pipe", name, L"--parent", std::to_wstring(GetCurrentProcessId()), L"--cache", cache, L"--libmpv", libmpv,
                   L"--ytdlp", ytdlp, L"--deno", deno},
                  false, &c)) {
        Check(false, "helper started for the pipe-close test");
        return;
    }
    const bool connected = ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
    Check(connected, "connected (pipe-close test)");
    const bool aliveAfterSilence = WaitForSingleObject(c.pi.hProcess, 8000) == WAIT_TIMEOUT;
    Check(aliveAfterSilence, "no pings for 8 s: the helper keeps running");
    DisconnectNamedPipe(p);
    CloseHandle(p);
    const DWORD w = WaitForSingleObject(c.pi.hProcess, 15000);
    DWORD code = 99;
    GetExitCodeProcess(c.pi.hProcess, &code);
    Check(w == WAIT_OBJECT_0 && code == 1, "the pipe closing ends the helper", "exit " + std::to_string(code));
    CloseHandle(c.pi.hThread);
    CloseHandle(c.pi.hProcess);
    CloseHandle(c.job);
    RemoveTree(cache);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring helper, libmpv, ytdlp, deno;
    bool online = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--online") online = true;
        else if (a == L"--medium") g_medium = true;
        else if (i + 1 < argc && a == L"--keep-log") g_keepLog = argv[++i];
        else if (i + 1 < argc && a == L"--helper") helper = argv[++i];
        else if (i + 1 < argc && a == L"--libmpv") libmpv = argv[++i];
        else if (i + 1 < argc && a == L"--ytdlp") ytdlp = argv[++i];
        else if (i + 1 < argc && a == L"--deno") deno = argv[++i];
    }
    if (helper.empty() || libmpv.empty() || ytdlp.empty() || deno.empty()) {
        fprintf(stderr, "usage: holo_media_lowil_test --helper <exe> --libmpv <dll> --ytdlp <exe> --deno <exe> [--online]\n");
        return 2;
    }
    // LocalLow\Melange\media-cache, as the core lays it out; removed again if this run created it.
    const std::wstring root = CacheRoot(), parent = root + L"\\media-cache";
    const bool hadRoot = GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool hadParent = GetFileAttributesW(parent.c_str()) != INVALID_FILE_ATTRIBUTES;
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(parent.c_str(), nullptr);
    const std::wstring st = parent + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-5e1f";
    CreateDirectoryW(st.c_str(), nullptr);
    if (!g_medium) SelftestAtLow(helper, libmpv, ytdlp, deno, st);
    RemoveTree(st);
    PipeAtLow(helper, libmpv, ytdlp, deno, parent, online);
    PipeClosed(helper, libmpv, ytdlp, deno, parent);
    if (!hadParent) RemoveDirectoryW(parent.c_str());
    if (!hadRoot) RemoveDirectoryW(root.c_str());
    printf("holo_media_lowil_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
