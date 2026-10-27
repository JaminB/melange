// Starting and watching holo-media.exe (helper.h).
#include "media/helper.h"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <cwctype>
#include <thread>

#include "core/log.h"
#include "core/thread_guard.h"

namespace melange::media::helper {
namespace {
std::wstring Upper(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towupper(c));
    return s;
}

bool StartsWith(const std::wstring& s, const wchar_t* p) { return s.rfind(p, 0) == 0; }

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string LastError(const char* what) { return std::string(what) + " (" + std::to_string(GetLastError()) + ")"; }

// One job for every helper of this game process. It is never closed, so it closes when the game exits, and the
// kernel then kills the helper and the yt-dlp and deno processes it started. A helper crash ends without a WER
// dialog over the game.
HANDLE Job() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (!j) return j;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li)) {
            LOG_WARN("[media] job limits: %lu", GetLastError());
        }
        return j;
    }();
    return job;
}

// A primary token like ours at Low integrity (S-1-16-4096).
HANDLE LowToken(std::string* err) {
    HANDLE self = nullptr, low = nullptr;
    const DWORD access = TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY;
    if (!OpenProcessToken(GetCurrentProcess(), access, &self)) {
        *err = LastError("OpenProcessToken");
        return nullptr;
    }
    const BOOL dup = DuplicateTokenEx(self, access, nullptr, SecurityImpersonation, TokenPrimary, &low);
    CloseHandle(self);
    if (!dup) {
        *err = LastError("DuplicateTokenEx");
        return nullptr;
    }
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-16-4096", &sid)) {
        *err = LastError("Low SID");
        CloseHandle(low);
        return nullptr;
    }
    TOKEN_MANDATORY_LABEL tml{};
    tml.Label.Attributes = SE_GROUP_INTEGRITY;
    tml.Label.Sid = sid;
    const BOOL set = SetTokenInformation(low, TokenIntegrityLevel, &tml, sizeof tml + GetLengthSid(sid));
    LocalFree(sid);
    if (!set) {
        *err = LastError("SetTokenInformation(TokenIntegrityLevel)");
        CloseHandle(low);
        return nullptr;
    }
    return low;
}
}  // namespace

EnvVars CurrentEnvironment() {
    EnvVars out;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return out;
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
        const std::wstring kv = p;
        const size_t eq = kv.find(L'=', 1);  // "=C:=C:\..." entries start with '='
        if (eq == std::wstring::npos || kv[0] == L'=') continue;
        out.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    }
    FreeEnvironmentStringsW(block);
    return out;
}

EnvVars BuildEnvironment(const EnvVars& parent, const std::wstring& cacheDir) {
    static const wchar_t* const kKeep[] = {L"SYSTEMROOT", L"WINDIR", L"COMSPEC", L"PATHEXT", L"SYSTEMDRIVE", L"OS",
                                           L"COMPUTERNAME", L"USERNAME", L"USERDOMAIN", L"NUMBER_OF_PROCESSORS",
                                           L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_ARCHITEW6432", L"PROCESSOR_IDENTIFIER",
                                           L"PROCESSOR_LEVEL", L"PROCESSOR_REVISION", L"PROGRAMDATA", L"PROGRAMFILES",
                                           L"PROGRAMFILES(X86)", L"PROGRAMW6432", L"COMMONPROGRAMFILES",
                                           L"COMMONPROGRAMFILES(X86)", L"COMMONPROGRAMW6432", L"PUBLIC", L"ALLUSERSPROFILE"};
    EnvVars out;
    std::wstring root;
    for (const auto& [k, v] : parent) {
        const std::wstring u = Upper(k);
        if (u == L"SYSTEMROOT") root = v;
        if (std::any_of(std::begin(kKeep), std::end(kKeep), [&](const wchar_t* x) { return u == x; })) out.emplace_back(k, v);
    }
    if (root.empty()) root = L"C:\\Windows";
    const std::wstring home = cacheDir + L"\\home";
    out.emplace_back(L"PATH", root + L"\\System32;" + root);
    out.emplace_back(L"HOME", home);
    out.emplace_back(L"USERPROFILE", home);
    out.emplace_back(L"APPDATA", home + L"\\AppData\\Roaming");
    out.emplace_back(L"LOCALAPPDATA", home + L"\\AppData\\Local");
    out.emplace_back(L"XDG_CONFIG_HOME", home + L"\\.config");
    out.emplace_back(L"XDG_CACHE_HOME", home + L"\\.cache");
    out.emplace_back(L"TEMP", cacheDir + L"\\tmp");
    out.emplace_back(L"TMP", cacheDir + L"\\tmp");
    out.emplace_back(L"DENO_DIR", cacheDir + L"\\deno");
    out.emplace_back(L"DENO_NO_UPDATE_CHECK", L"1");
    out.emplace_back(L"DENO_NO_PROMPT", L"1");
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return Upper(a.first) < Upper(b.first); });
    return out;
}

std::wstring EnvironmentBlock(const EnvVars& env) {
    std::wstring b;
    for (const auto& [k, v] : env) {
        b += k;
        b += L'=';
        b += v;
        b += L'\0';
    }
    b += L'\0';
    if (env.empty()) b += L'\0';
    return b;
}

std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == a.size()) {
            q.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            q.append(slashes * 2 + 1, L'\\');
            q += L'"';
        } else {
            q.append(slashes, L'\\');
            q += a[i];
        }
    }
    return q + L"\"";
}

std::wstring CommandLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring c = QuoteArg(exe);
    for (const std::wstring& a : args) c += L" " + QuoteArg(a);
    return c;
}

std::string RandomHex(int bytes) {
    uint8_t buf[32] = {};
    bytes = std::clamp(bytes, 1, 32);
    if (BCryptGenRandom(nullptr, buf, static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        for (int i = 0; i < bytes; ++i) buf[i] = static_cast<uint8_t>((t.QuadPart >> ((i % 8) * 8)) ^ (GetCurrentProcessId() * (i + 1)));
    }
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < bytes; ++i) {
        s += hex[buf[i] >> 4];
        s += hex[buf[i] & 15];
    }
    return s;
}

bool PrepareCacheDir(const std::wstring& dir, std::string* err) {
    // The parent chain may not exist yet (a fresh LocalLow\Melange); create it one level at a time.
    for (size_t p = dir.find(L'\\', 3); p != std::wstring::npos; p = dir.find(L'\\', p + 1)) CreateDirectoryW(dir.substr(0, p).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    for (const wchar_t* sub : {L"home", L"home\\AppData", L"home\\AppData\\Roaming", L"home\\AppData\\Local", L"home\\.config",
                               L"home\\.cache", L"deno", L"mpvhome", L"tmp", L"bt"}) {
        const std::wstring p = dir + L"\\" + sub;
        if (!CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            *err = "cache folder " + Utf8(p) + " (" + std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- Process
Process::~Process() {
    if (proc_) CloseHandle(proc_);
}

bool Process::Alive() const { return proc_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT; }

void Process::Kill(uint32_t code) {
    if (proc_) TerminateProcess(proc_, code);
}

bool Process::Launch(const LaunchSpec& spec, const std::wstring& pipeName, std::string* err) {
    std::vector<std::wstring> args = {proto::arg::kPipe, pipeName};
    args.insert(args.end(), spec.args.begin(), spec.args.end());
    std::wstring cmd = CommandLine(spec.exe, args);
    const std::wstring env = EnvironmentBlock(BuildEnvironment(CurrentEnvironment(), spec.cacheDir));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    // No inherited handles; suspended until it is in the job, so no child it starts can escape the job (T3).
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT |
                        CREATE_DEFAULT_ERROR_MODE;
    const wchar_t* cwd = spec.cacheDir.empty() ? nullptr : spec.cacheDir.c_str();
    BOOL ok;
    if (spec.lowIntegrity) {
        HANDLE tok = LowToken(err);
        if (!tok) return false;
        ok = CreateProcessAsUserW(tok, spec.exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags,
                                  const_cast<wchar_t*>(env.c_str()), cwd, &si, &pi);
        if (!ok) *err = LastError("CreateProcessAsUser");
        CloseHandle(tok);
    } else {
        ok = CreateProcessW(spec.exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, const_cast<wchar_t*>(env.c_str()),
                            cwd, &si, &pi);
        if (!ok) *err = LastError("CreateProcess");
    }
    if (!ok) return false;
    HANDLE job = Job();
    inJob_ = job && AssignProcessToJobObject(job, pi.hProcess);
    if (!inJob_) LOG_WARN("[media] the helper is not in the job (%lu); it still exits with the game (--parent)", GetLastError());
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        *err = LastError("ResumeThread");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    low_ = spec.lowIntegrity;
    return true;
}

// ---------------------------------------------------------------- Session
bool Session::Start(const LaunchSpec& spec, const pipe::HelloCmd& hello, const pipeio::Timings& t, std::string* err) {
    const std::string rnd = RandomHex(8);
    const std::wstring name = L"\\\\.\\pipe\\melange-holo-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::wstring(rnd.begin(), rnd.end());
    conn_ = pipeio::Connection::Create(name, spec.lowIntegrity, err);
    if (!conn_) return false;
    auto proc = std::make_shared<Process>();
    if (!proc->Launch(spec, name, err)) {
        conn_->Close("launch failed");
        conn_.reset();
        return false;
    }
    proc_ = proc;
    conn_->Start(proc->Pid(), t, [proc] { return proc->Alive(); }, [proc] { proc->Kill(0xDEAD); });
    conn_->Send(pipe::Encode(hello));
    return true;
}

void Session::Send(const pipe::Message& m) {
    if (conn_ && !stopped_) conn_->Send(pipe::Encode(m));
}

void Session::Drain(std::vector<pipe::Message>* out) {
    if (!conn_) return;
    const size_t from = out->size();
    conn_->In().Drain(out);
    for (size_t i = from; i < out->size(); ++i)
        if (auto* h = std::get_if<pipe::HelloEvt>(&(*out)[i])) {
            hello_ = *h;
            gotHello_ = true;
        }
}

Session::State Session::GetState() {
    if (!conn_ || !proc_ || stopped_) return State::Broken;
    if (conn_->GetState() == pipeio::Connection::State::Closed) return State::Broken;
    if (!proc_->Alive()) {
        conn_->Close("the helper exited");
        return State::Broken;
    }
    return conn_->GetState() == pipeio::Connection::State::Connected && gotHello_ ? State::Ready : State::Starting;
}

std::string Session::BrokenReason() const {
    if (!conn_) return "not started";
    std::string why = conn_->CloseReason();
    DWORD code = 0;
    if (proc_ && proc_->Handle() && GetExitCodeProcess(proc_->Handle(), &code) && code != STILL_ACTIVE) {
        char buf[48];
        snprintf(buf, sizeof buf, " (exit 0x%lx)", code);
        why += buf;
    }
    return why;
}

void Session::Stop(uint32_t graceMs) {
    if (stopped_ || !conn_) return;
    conn_->Send(pipe::Encode(pipe::QuitCmd{}));
    stopped_ = true;
    auto conn = conn_;
    auto proc = proc_;
    try {
        std::thread([conn, proc, graceMs] {
            GuardedThreadBody("media helper stop", [&] {
                if (proc && proc->Handle() && WaitForSingleObject(proc->Handle(), graceMs) == WAIT_TIMEOUT) proc->Kill(0);
                conn->Close("stopped");
            });
        }).detach();
    } catch (const std::system_error&) {
        // No new thread while the game exits (~Session of the global session runs then): end the helper now. The pipe's
        // own threads are gone already, so the connection is left alone.
        if (proc) proc->Kill(0);
    }
}
}  // namespace melange::media::helper
