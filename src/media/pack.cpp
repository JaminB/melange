// The media pack installer and the yt-dlp lane (pack.h): the state everyone polls, the media worker thread that runs
// installs, lane checks, the start sweep and uninstalls one at a time, the launch-time check (re-hash with the files
// held, the Authenticode publisher of holo-media.exe), the [Media] gates and the match throttle. The disk and network
// steps are game-free in pack_fetch.cpp, pack_extract.cpp and ytdlp_lane.cpp (pack_internal.h).
#include "media/pack.h"

#include <windows.h>

#include <shlobj.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/thread_guard.h"
#include "media/config_keys.h"
#include "media/pack_internal.h"
#include "media/pack_manifest.h"
#include "net/net.h"
#include "version.h"

#ifndef FILE_ATTRIBUTE_PINNED
#define FILE_ATTRIBUTE_PINNED 0x00080000
#endif
#ifndef FILE_ATTRIBUTE_UNPINNED
#define FILE_ATTRIBUTE_UNPINNED 0x00100000
#endif

namespace melange::media::pack {
namespace {
constexpr wchar_t kInstallMutex[] = L"Local\\melange-media-install";
constexpr wchar_t kLaneMutex[] = L"Local\\melange-media-install-ytdlp";

enum class Task : uint8_t { Sweep, Install, Lane, LaneFirst, LaneForce, Uninstall };

std::mutex g_mx;  // guards everything below that is not atomic
std::condition_variable g_cv;
std::deque<Task> g_tasks;
bool g_workerStarted = false;
Info g_info;
std::string g_requestedBy;
cfg::Media g_cfg;
core::Layout g_layout;
logic::Manifest g_manifest;
bool g_haveManifest = false;
core::LaneState g_lane;
std::once_flag g_once;
std::atomic<bool> g_cancel{false};
std::atomic<uint32_t> g_throttleKBps{0};
std::atomic<int> g_held{0};

logic::Manifest Compiled() {
    logic::Manifest m;
    m.version = manifest::kPackVersion;
    m.zipUrl = manifest::kZipUrl;
    m.zipSize = manifest::kZipSize;
    m.zipSha256 = manifest::kZipSha256;
    m.laneUrl = manifest::kYtdlpLaneUrl;
    m.dev = manifest::kDev;
    if (manifest::kHavePack)
        for (const manifest::File& f : manifest::kFiles) m.files.push_back({f.path, f.size, f.sha256});
    return m;
}

// x64 Windows (or ARM64, which emulates x64): the helper is a 64-bit process. IsWow64Process2 needs Windows 10 1709;
// older systems fall back to IsWow64Process (Critique T5).
bool HostRunsX64(const char** how) {
    using Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (fn) {
        USHORT proc = 0, native = 0;
        if (fn(GetCurrentProcess(), &proc, &native)) {
            *how = native == IMAGE_FILE_MACHINE_ARM64 ? "ARM64 (x64 emulated)" : "x64";
            return native == IMAGE_FILE_MACHINE_AMD64 || native == IMAGE_FILE_MACHINE_ARM64;
        }
    }
    BOOL wow = FALSE;
    *how = "x64 (IsWow64Process)";
    return IsWow64Process(GetCurrentProcess(), &wow) && wow;
}

std::wstring MediaRoot() {
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = docs;
    if (docs) CoTaskMemFree(docs);
    return out.empty() ? out : out + L"\\Melange\\media";
}

// Documents is often redirected into OneDrive (Known Folder Move). Pinned content is never dehydrated to an online-only
// placeholder, so the helper's files stay local (Critique T4). Only under a OneDrive root; elsewhere the attribute means
// nothing and may be refused.
void PinIfOneDrive(const std::wstring& dir) {
    for (const wchar_t* var : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
        wchar_t root[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(var, root, MAX_PATH);
        if (!n || n >= MAX_PATH || dir.size() <= n || _wcsnicmp(dir.c_str(), root, n) != 0 || dir[n] != L'\\') continue;
        const DWORD a = GetFileAttributesW(dir.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) return;
        if (SetFileAttributesW(dir.c_str(), (a | FILE_ATTRIBUTE_PINNED) & ~FILE_ATTRIBUTE_UNPINNED))
            LOG_INFO("[media] %s is in OneDrive: pinned it so its files stay on this PC", core::Narrow(dir).c_str());
        else
            LOG_WARN("[media] %s is in OneDrive and could not be pinned (error %lu)", core::Narrow(dir).c_str(), GetLastError());
        return;
    }
}

// A named mutex across game instances (LocalNet A+B on one machine). Waits in steps so a cancel ends the wait.
struct NamedMutex {
    HANDLE h = nullptr;
    bool owned = false;
    bool Acquire(const wchar_t* name, const std::atomic<bool>* cancel, DWORD maxMs = INFINITE) {
        h = CreateMutexW(nullptr, FALSE, name);
        if (!h) return false;
        const ULONGLONG start = GetTickCount64();
        for (;;) {
            const DWORD w = WaitForSingleObject(h, maxMs == 0 ? 0 : 250);
            if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) return owned = true;
            if (w != WAIT_TIMEOUT || (cancel && cancel->load())) return false;
            if (maxMs != INFINITE && GetTickCount64() - start >= maxMs) return false;
        }
    }
    ~NamedMutex() {
        if (owned) ReleaseMutex(h);
        if (h) CloseHandle(h);
    }
};

void SetState(State s, const char* error = nullptr) {
    std::lock_guard lk(g_mx);
    g_info.state = s;
    g_info.error = error ? error : "";
    if (s != State::Downloading && s != State::Verifying && s != State::Installing) g_info.bytes = g_info.total = 0;
    if (s == State::Ready) g_info.progress = 1;
}

void SetError(const char* error) {
    std::lock_guard lk(g_mx);
    g_info.error = error;
}

// wum.media.pack().error: the documented values ("hash", "disk-space", "network", "cancelled", "blocked") plus "disk"
// (cannot write), "in-use" and "url" ([Media] PackUrl refused). A refused zip or pack.json is "hash": the bytes are not
// the pinned ones. The detail goes to the log.
const char* PublicError(const std::string& e) {
    for (const char* known : {"hash", "disk-space", "network", "cancelled", "blocked", "disk", "in-use", "url"})
        if (e == known) return known;
    return e == "zip" || e == "manifest" ? "hash" : "error";
}

core::Hooks MakeHooks(bool lane) {
    core::Hooks h;
    h.cancel = lane ? nullptr : &g_cancel;  // CancelInstall stops the pack download, never a background lane check
    h.userAgent = std::string("Melange/") + MELANGE_VERSION;
    h.throttleKBps = [] { return g_throttleKBps.load(); };
    h.progress = [](uint64_t done, uint64_t total) {
        std::lock_guard lk(g_mx);
        g_info.bytes = done;
        g_info.total = total;
        g_info.progress = total ? static_cast<double>(done) / static_cast<double>(total) : 0;
    };
    h.phase = [lane](const char* phase) {
        std::lock_guard lk(g_mx);
        if (lane) {
            // The lane runs beside a ready pack: "updating" only while a new yt-dlp actually downloads.
            if (!strcmp(phase, "downloading")) g_info.state = State::Updating;
            return;
        }
        g_info.state = !strcmp(phase, "downloading") ? State::Downloading
                       : !strcmp(phase, "verifying") ? State::Verifying
                                                     : State::Installing;
        g_info.progress = 0;
    };
    return h;
}

void DoSweep() {
    NamedMutex mx;
    // Only when no other instance installs right now: its <ver>.tmp is live.
    if (mx.Acquire(kInstallMutex, nullptr, 0)) core::Sweep(g_layout);
    panel::Register();
}

void DoLane(Task t) {
    {
        std::lock_guard lk(g_mx);
        if (g_info.state != State::Ready) return;
        if (!g_cfg.allowDownload) return;
        if (t != Task::LaneFirst && !g_cfg.ytdlpUpdate && !g_lane.exe.empty()) return;  // [Media] YtDlpUpdate=0
        if (t != Task::LaneFirst && !g_lane.exe.empty() && !logic::LaneCheckDue(core::UnixNow(), g_lane.lastCheck, t == Task::LaneForce))
            return;
    }
    NamedMutex mx;
    if (!mx.Acquire(kLaneMutex, nullptr)) return;
    std::string action, error, detail;
    core::LaneState st;
    const bool ok = core::UpdateLane(g_layout, g_manifest.laneUrl, cfg::kAllowedUrlPrefix, MakeHooks(true), &st, &action, &error, &detail);
    {
        std::lock_guard lk(g_mx);
        if (g_info.state == State::Updating) g_info.state = State::Ready;
        g_lane = st.lane.serial || !st.exe.empty() ? st : core::ReadLane(g_layout);
        g_info.ytdlp = g_lane.exe.empty() ? "" : g_lane.lane.version;
    }
    if (ok)
        LOG_INFO("[media] yt-dlp lane: %s (yt-dlp %s, serial %llu)", action.c_str(), g_lane.lane.version.c_str(), g_lane.lane.serial);
    else
        LOG_WARN("[media] yt-dlp lane check failed (%s): %s; keeping yt-dlp %s", error.c_str(), detail.c_str(),
                 g_lane.lane.version.empty() ? "(none)" : g_lane.lane.version.c_str());
}

void EnqueueLocked(Task t);

void DoInstall() {
    cfg::Media c;
    {
        std::lock_guard lk(g_mx);
        c = g_cfg;
    }
    NamedMutex mx;
    if (!mx.Acquire(kInstallMutex, &g_cancel)) {
        SetState(State::Error, g_cancel ? "cancelled" : "disk");
        return;
    }
    if (core::PackPresent(g_layout, g_manifest)) {
        LOG_INFO("[media] the media pack %s is already installed (another game instance)", g_manifest.version.c_str());
        SetState(State::Ready);
    } else {
        std::string url, why, error, detail;
        if (!logic::ChoosePackUrl(c.packUrl, g_manifest, cfg::kAllowedUrlPrefix, &url, &why)) {
            LOG_ERROR("[media] pack install refused: %s", why.c_str());
            SetState(State::Error, "url");
            return;
        }
        core::MakeDirs(g_layout.root);
        PinIfOneDrive(g_layout.root);
        LOG_INFO("[media] downloading the media pack %s from %s", g_manifest.version.c_str(), url.c_str());
        if (!core::InstallPack(g_layout, g_manifest, url, MakeHooks(false), &error, &detail)) {
            LOG_ERROR("[media] pack install failed (%s): %s", error.c_str(), detail.c_str());
            SetState(State::Error, PublicError(error));
            return;
        }
        PinIfOneDrive(g_layout.PackDir(g_manifest.version));
        LOG_INFO("[media] media pack %s installed in %s", g_manifest.version.c_str(),
                 core::Narrow(g_layout.PackDir(g_manifest.version)).c_str());
        SetState(State::Ready);
    }
    // The first install also fetches yt-dlp (the user's click covered both).
    std::lock_guard lk(g_mx);
    EnqueueLocked(Task::LaneFirst);
}

void DoUninstall() {
    if (g_held.load() > 0) {
        LOG_WARN("[media] uninstall refused: the media helper is starting or running");
        SetError("in-use");
        return;
    }
    NamedMutex mx;
    if (!mx.Acquire(kInstallMutex, nullptr, 5000)) {
        SetError("busy");
        return;
    }
    std::string error;
    if (!core::Uninstall(g_layout, &error)) {
        LOG_WARN("[media] uninstall refused: a pack file is in use (close the player, or the other game instance)");
        SetError(error.c_str());
        return;
    }
    LOG_INFO("[media] media pack uninstalled (%s)", core::Narrow(g_layout.root).c_str());
    std::lock_guard lk(g_mx);
    g_lane = {};
    g_info.ytdlp.clear();
    g_info.state = State::Missing;
    g_info.error.clear();
    g_info.progress = 0;
}

void Worker() {
    for (;;) {
        Task t;
        {
            std::unique_lock lk(g_mx);
            g_cv.wait(lk, [] { return !g_tasks.empty(); });
            t = g_tasks.front();
            g_tasks.pop_front();
        }
        GuardedThreadBody("media", [&] {
            switch (t) {
                case Task::Sweep: DoSweep(); break;
                case Task::Install: DoInstall(); break;
                case Task::Lane:
                case Task::LaneFirst:
                case Task::LaneForce: DoLane(t); break;
                case Task::Uninstall: DoUninstall(); break;
            }
        });
    }
}

// Caller holds g_mx. A task already queued is not queued twice.
void EnqueueLocked(Task t) {
    for (Task q : g_tasks)
        if (q == t) return;
    if (!g_workerStarted) {
        g_workerStarted = true;
        std::thread([] { GuardedThreadBody("media", &Worker); }).detach();
    }
    g_tasks.push_back(t);
    g_cv.notify_one();
}

// The first call (any thread, usually the media host's frame poll) resolves the folders, reads [Media], and decides
// the state from a few file sizes (no hashing, so it is cheap enough for the game thread); the sweep and the panel go to
// the worker.
void EnsureStarted() {
    std::call_once(g_once, [] {
        const logic::Manifest m = Compiled();
        std::string why;
        const bool have = manifest::kHavePack && logic::CheckManifest(m, &why);
        if (manifest::kHavePack && !have) LOG_ERROR("[media] the compiled pack manifest is invalid: %s", why.c_str());
        if (manifest::kDev) LOG_WARN("[media] DEVELOPER media pack manifest compiled in (%s): not for release", manifest::kZipUrl);
        const cfg::Media c = cfg::ReadMedia();
        const bool enabled = config::GetInt("Media", "Enabled", 1) != 0;
        const char* how = "";
        const bool x64 = HostRunsX64(&how);
        const std::wstring root = MediaRoot();
        std::lock_guard lk(g_mx);
        g_manifest = m;
        g_haveManifest = have;
        g_cfg = c;
        g_layout.root = root;
        g_info.version = manifest::kPackVersion;
        g_info.downloadMB = manifest::kDownloadMB;
        g_info.diskMB = manifest::kDiskMB;
        g_info.folder = root.empty() ? root : g_layout.PackDir(m.version);
        if (!enabled) {
            g_info.state = State::Disabled;
            return;
        }
        if (!x64) {
            g_info.state = State::Unsupported;
            LOG_INFO("[media] this Windows has no x64 support: the media pack is unavailable");
            return;
        }
        if (root.empty()) {
            g_info.state = State::Error;
            g_info.error = "disk";
            LOG_ERROR("[media] the Documents folder could not be resolved");
            return;
        }
        g_lane = core::ReadLane(g_layout);
        g_info.ytdlp = g_lane.exe.empty() ? "" : g_lane.lane.version;
        g_info.state = have && core::PackPresent(g_layout, m) ? State::Ready : State::Missing;
        LOG_INFO("[media] media pack %s: %s (%s, yt-dlp %s)", m.version.c_str(), StateName(g_info.state), how,
                 g_info.ytdlp.empty() ? "none" : g_info.ytdlp.c_str());
        EnqueueLocked(Task::Sweep);
    });
}

// Main thread only (the net session's phase is a game read): the match throttle for downloads in flight.
void SampleThrottle() {
    if (GetCurrentThreadId() != events::MainThreadId()) return;
    const wum::SessionPhase ph = wum::Phase();
    uint32_t kbps = 0;
    if (ph == wum::SessionPhase::Match || ph == wum::SessionPhase::Loading) {
        std::lock_guard lk(g_mx);
        kbps = static_cast<uint32_t>(g_cfg.matchDownloadKBps);
    }
    g_throttleKBps = kbps;
}

// ---------------------------------------------------------------- Authenticode (holo-media.exe vs melange.asi)
struct Signer {
    bool valid = false;
    std::string subject, issuerOrg, error;
};

std::string NameString(PCCERT_CONTEXT c, bool issuerOrg) {
    if (issuerOrg) {
        const DWORD n = CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, CERT_NAME_ISSUER_FLAG, const_cast<char*>(szOID_ORGANIZATION_NAME), nullptr, 0);
        if (n <= 1) return {};
        std::wstring w(n, L'\0');
        CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, CERT_NAME_ISSUER_FLAG, const_cast<char*>(szOID_ORGANIZATION_NAME), w.data(), n);
        w.resize(wcslen(w.c_str()));
        return core::Narrow(w);
    }
    const DWORD n = CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(n, L'\0');
    CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR, w.data(), n);
    w.resize(wcslen(w.c_str()));
    return core::Narrow(w);
}

// The same check as Melange.exe's updater (src/launcher/updater.cpp ReadSigner), on an already open handle so the
// verified bytes are the held ones. No revocation lookups (offline play); the chain must reach a trusted root.
Signer ReadSigner(const std::wstring& path, HANDLE file) {
    Signer s;
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path.c_str();
    fi.hFile = file;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG st = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    s.valid = st == ERROR_SUCCESS;
    if (!s.valid) {
        char b[64];
        snprintf(b, sizeof b, "0x%08lx", static_cast<unsigned long>(st));
        s.error = st == TRUST_E_NOSIGNATURE ? "not signed" : st == TRUST_E_BAD_DIGEST ? "changed after signing" : std::string(b);
    }
    if (wd.hWVTStateData) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        if (pc && pc->pCert) {
            s.subject = NameString(pc->pCert, false);
            s.issuerOrg = NameString(pc->pCert, true);
        }
        wd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    }
    return s;
}

// melange.asi's own publisher, read once. An unsigned (developer) melange.asi has no subject.
const Signer& OwnSigner() {
    static Signer s;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH * 2] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&OwnSigner), &self) &&
            GetModuleFileNameW(self, path, MAX_PATH * 2)) {
            HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            s = ReadSigner(path, f == INVALID_HANDLE_VALUE ? nullptr : f);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            if (!s.valid) s.subject.clear();
        }
    });
    return s;
}
}  // namespace

const char* StateName(State s) {
    switch (s) {
        case State::Missing: return "missing";
        case State::Downloading: return "downloading";
        case State::Verifying: return "verifying";
        case State::Installing: return "installing";
        case State::Ready: return "ready";
        case State::Updating: return "updating";
        case State::Error: return "error";
        case State::Unsupported: return "unsupported";
        case State::Disabled: return "disabled";
    }
    return "?";
}

Info Get() {
    EnsureStarted();
    SampleThrottle();
    std::lock_guard lk(g_mx);
    return g_info;
}

bool Install(const char* modId, std::string* reason) {
    EnsureStarted();
    auto refuse = [&](const char* r) {
        if (reason) *reason = r;
        return false;
    };
    const cfg::Media c = cfg::ReadMedia();
    std::lock_guard lk(g_mx);
    g_cfg = c;  // a changed AllowDownload or PackUrl counts from this click on
    switch (g_info.state) {
        case State::Disabled: return refuse("disabled");
        case State::Unsupported: return refuse("unsupported-os");
        case State::Ready:
        case State::Updating: return refuse("ready");
        case State::Downloading:
        case State::Verifying:
        case State::Installing: return refuse("busy");
        case State::Missing:
        case State::Error: break;
    }
    if (!g_cfg.allowDownload) return refuse("blocked");
    if (!g_haveManifest) return refuse("unavailable");  // no published pack compiled into this build (before R1)
    for (Task q : g_tasks)
        if (q == Task::Install || q == Task::Uninstall) return refuse("busy");
    g_requestedBy = modId ? modId : "?";
    g_cancel = false;
    LOG_INFO("[media] pack install requested by %s (user click)", g_requestedBy.c_str());
    g_info.state = State::Downloading;
    g_info.error.clear();
    g_info.progress = 0;
    g_info.bytes = 0;
    g_info.total = g_manifest.zipSize;
    EnqueueLocked(Task::Install);
    return true;
}

void CancelInstall() {
    std::lock_guard lk(g_mx);
    g_cancel = true;
    for (auto it = g_tasks.begin(); it != g_tasks.end();)
        if (*it == Task::Install) {
            it = g_tasks.erase(it);
            g_info.state = State::Error;
            g_info.error = "cancelled";
        } else {
            ++it;
        }
}

bool ReadyPaths(Paths* out) {
    EnsureStarted();
    std::lock_guard lk(g_mx);
    // Ready, or Updating: a yt-dlp download beside the pack leaves the installed one usable.
    if (g_info.state != State::Ready && g_info.state != State::Updating) return false;
    Paths p;
    p.dir = g_layout.PackDir(g_manifest.version);
    p.helperExe = p.dir + L"\\holo-media.exe";
    p.libmpv = p.dir + L"\\libmpv-2.dll";
    p.deno = p.dir + L"\\deno.exe";
    p.ytdlp = g_lane.exe;  // "" until the lane installed one: YouTube items then fail, torrents still play
    if (g_cfg.allowCustomLibmpv && !g_cfg.customLibmpv.empty()) {
        p.libmpv = g_cfg.customLibmpv;
        p.customLibmpv = true;
    }
    *out = std::move(p);
    return true;
}

bool VerifyForLaunch(const Paths& p, Held* out, std::string* err) {
    EnsureStarted();
    out->handles.clear();
    core::Layout layout;
    logic::Manifest m;
    core::LaneState lane;
    {
        std::lock_guard lk(g_mx);
        layout = g_layout;
        m = g_manifest;
        lane = g_lane;
    }
    std::vector<HANDLE> held;
    std::string detail;
    auto fail = [&](const char* e, bool corrupt) {
        for (HANDLE h : held) CloseHandle(h);
        if (err) *err = e;
        LOG_ERROR("[media] helper launch refused (%s): %s", e, detail.c_str());
        if (corrupt) SetState(State::Error, "hash");  // a changed pack is reinstalled by the next install()
        return false;
    };
    auto hold = [&](const std::wstring& path, const std::wstring& inside, uint64_t size, const std::string& sha) {
        HANDLE h = INVALID_HANDLE_VALUE;
        if (!core::OpenHashed(path, inside, size, sha, &h, &detail)) return false;
        held.push_back(h);
        return true;
    };
    for (const char* name : {"holo-media.exe", "libmpv-2.dll", "deno.exe"}) {
        if (p.customLibmpv && !strcmp(name, "libmpv-2.dll")) continue;
        const logic::File* f = m.Find(name);
        if (!f || !hold(p.dir + L"\\" + core::Widen(name), p.dir, f->size, f->sha256)) return fail("hash", true);
    }
    if (p.customLibmpv) LOG_WARN("[media] loading a user-supplied libmpv, unverified: %s", core::Narrow(p.libmpv).c_str());
    if (!p.ytdlp.empty()) {
        if (p.ytdlp != lane.exe || !hold(p.ytdlp, layout.LaneRoot(), lane.lane.size, lane.lane.sha256)) {
            detail = "yt-dlp.exe: " + detail;
            {
                std::lock_guard lk(g_mx);
                g_lane.exe.clear();  // the lane reinstalls it at the next check
                g_info.ytdlp.clear();
            }
            return fail("hash", false);
        }
    }
    // pack.json changed since the install was recorded: the whole set again, once.
    const uint64_t rec = core::RecordedPackJsonTime(layout, m);
    if (!rec || rec != core::PackJsonTime(layout, m)) {
        if (!core::VerifyPackFiles(layout, m, &detail)) return fail("hash", true);
        core::RecordInstalled(layout, m);
        LOG_INFO("[media] the media pack was re-verified (pack.json changed)");
    }
    const Signer& own = OwnSigner();
    if (own.subject.empty()) {
        static std::once_flag logged;
        std::call_once(logged, [] { LOG_WARN("[media] melange.asi is unsigned (a developer build): holo-media.exe's signature is not checked"); });
    } else {
        const Signer s = ReadSigner(p.helperExe, held.front());
        if (!s.valid) {
            detail = "holo-media.exe is not validly signed (" + s.error + ")";
            return fail("signature", true);
        }
        if (s.subject != own.subject || s.issuerOrg != own.issuerOrg) {
            detail = "holo-media.exe is signed by \"" + s.subject + "\", not by \"" + own.subject + "\" like melange.asi";
            return fail("signature", true);
        }
    }
    for (HANDLE h : held) out->handles.push_back(h);
    g_held.fetch_add(1);
    return true;
}

void ReleaseHeld(Held* h) {
    if (!h) return;
    if (!h->handles.empty()) g_held.fetch_sub(1);
    for (void* x : h->handles) CloseHandle(static_cast<HANDLE>(x));
    h->handles.clear();
}

void CheckYtdlpLane(bool force) {
    EnsureStarted();
    std::lock_guard lk(g_mx);
    if (g_info.state != State::Ready || !g_cfg.allowDownload) return;
    if (!g_cfg.ytdlpUpdate && !g_lane.exe.empty()) return;  // [Media] YtDlpUpdate=0: only a missing yt-dlp is fetched
    if (!g_lane.exe.empty() && !logic::LaneCheckDue(core::UnixNow(), g_lane.lastCheck, force)) return;
    EnqueueLocked(force ? Task::LaneForce : Task::Lane);
}

namespace panel {
std::string RequestedBy() {
    std::lock_guard lk(g_mx);
    return g_requestedBy;
}
bool InUse() { return g_held.load() > 0; }
void Uninstall() {
    std::lock_guard lk(g_mx);
    EnqueueLocked(Task::Uninstall);
}
}  // namespace panel
}  // namespace melange::media::pack
