#pragma once
// Starting and watching holo-media.exe (design 1.1.4, Critique S2/T1/T3): a Low-integrity process with a rebuilt
// environment and no inherited handles, created suspended, put in a kill-on-close job and only then resumed, with the
// pipe server (pipe.h) it must connect to. Game-free: media.cpp builds the launch from the pack and the ini, and
// media_selftest drives tests/fake_helper.exe through the same code.
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "media/pipe.h"

namespace melange::media::helper {
using EnvVars = std::vector<std::pair<std::wstring, std::wstring>>;

// The helper's environment, built from scratch (S2): only the system variables Windows programs need from `parent`
// (SystemRoot, windir, ComSpec, PATHEXT, the processor and OS ones, COMPUTERNAME, USERNAME, SystemDrive, ProgramData,
// ProgramFiles*, PUBLIC, LOCALAPPDATA), PATH reduced to System32 and the Windows folder, every home and temp folder
// redirected into the instance's cache (HOME, USERPROFILE stays, APPDATA, XDG_CONFIG_HOME, TEMP, TMP), DENO_DIR,
// DENO_NO_UPDATE_CHECK and DENO_NO_PROMPT. Nothing named PYTHON*, YTDLP*, YT_DLP*, MPV_*, DENO_* (other than ours) or
// LUA_* survives. Sorted, as CreateProcess expects.
EnvVars BuildEnvironment(const EnvVars& parent, const std::wstring& cacheDir);
EnvVars CurrentEnvironment();
// The double-NUL-terminated block for CREATE_UNICODE_ENVIRONMENT.
std::wstring EnvironmentBlock(const EnvVars& env);
// One argument quoted by the CommandLineToArgvW rules; the full command line.
std::wstring QuoteArg(const std::wstring& a);
std::wstring CommandLine(const std::wstring& exe, const std::vector<std::wstring>& args);

std::string RandomHex(int bytes);  // BCryptGenRandom

// The cache subfolders the helper expects (home, deno, mpvhome, tmp, bt); created before the launch.
bool PrepareCacheDir(const std::wstring& dir, std::string* err);

struct LaunchSpec {
    std::wstring exe;
    std::vector<std::wstring> args;  // after --pipe <name>, which Session adds
    std::wstring cacheDir;           // this instance's folder (created by PrepareCacheDir)
    bool lowIntegrity = true;
};

// The process: suspended, job, resume. Keeps its handle; the job (process-wide, never closed) kills the helper and
// every child it starts when the game exits.
class Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    bool Launch(const LaunchSpec& spec, const std::wstring& pipeName, std::string* err);
    uint32_t Pid() const { return pid_; }
    void* Handle() const { return proc_; }
    bool Alive() const;
    void Kill(uint32_t code);
    bool InJob() const { return inJob_; }
    bool Low() const { return low_; }

private:
    void* proc_ = nullptr;
    uint32_t pid_ = 0;
    bool inJob_ = false, low_ = false;
};

// A started helper and its pipe. Main thread owns it; the pipe threads only touch the shared Connection and a
// duplicate of the process handle (to judge "alive" and to kill on a hang).
class Session {
public:
    enum class State : uint8_t { Starting, Ready, Broken };
    Session() = default;
    ~Session() { Stop(500); }  // never leaves a helper running without an owner
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Creates the pipe \\.\pipe\melange-holo-<pid>-<rand64>, launches, starts the I/O, queues `hello`.
    bool Start(const LaunchSpec& spec, const pipe::HelloCmd& hello, const pipeio::Timings& t, std::string* err);
    void Send(const pipe::Message& m);
    void Drain(std::vector<pipe::Message>* out);  // and notes the helper's hello
    State GetState();
    bool Hung() const { return conn_ && conn_->Hung(); }
    std::string BrokenReason() const;
    // Asks the helper to quit, then (after `graceMs`, from a worker thread) kills it if it is still there.
    void Stop(uint32_t graceMs = 2000);
    const pipe::HelloEvt& Hello() const { return hello_; }
    bool GotHello() const { return gotHello_; }
    uint32_t Pid() const { return proc_ ? proc_->Pid() : 0; }
    bool Low() const { return proc_ && proc_->Low(); }
    void* ProcessHandle() const { return proc_ ? proc_->Handle() : nullptr; }
    pipeio::Connection* Conn() { return conn_.get(); }

private:
    std::shared_ptr<pipeio::Connection> conn_;
    std::shared_ptr<Process> proc_;
    pipe::HelloEvt hello_;
    bool gotHello_ = false, stopped_ = false;
};

// At most one automatic restart per window (proto::kRestartMinGapMs); after that the helper stays dead.
struct RestartPolicy {
    double windowMs = proto::kRestartMinGapMs;
    double lastMs = -1e300;
    bool Allow(double nowMs) {
        if (nowMs - lastMs < windowMs) return false;
        lastMs = nowMs;
        return true;
    }
};
}  // namespace melange::media::helper
