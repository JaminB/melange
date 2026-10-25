#pragma once
// What the helper does for its own containment (Critique S1/S2/T2), on top of what the core sets up (Low integrity
// token, job object, rebuilt environment): it rebuilds the environment again (defence in depth: no PYTHON*, YTDLP*,
// MPV_HOME; TEMP, HOME, DENO_DIR in its cache), turns on process mitigations, puts itself and every child in its own
// kill-on-close job, holds its cache folder's lock file and sweeps dead instances.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace holo::sandbox {
// The token's integrity RID: 0x1000 Low, 0x2000 Medium, 0x3000 High (0 if unknown).
uint32_t IntegrityRid(HANDLE token = nullptr);
const char* IntegrityName(uint32_t rid);

// Creates <cache>\{mpvhome,home,deno,tmp} and rewrites this process's environment for its children.
bool PrepareCache(const std::wstring& cache, std::string* why);
void ScrubEnvironment(const std::wstring& cache);
void ApplyMitigations();
// A kill-on-close job holding this process, so every yt-dlp and deno it starts dies with it (nested in the core's).
void JoinOwnJob();

// Writes the Windows trusted root certificates (current user's view, which includes the machine's and policy roots)
// as PEM, for mpv's tls-ca-file. False when the store yields nothing.
bool WriteRootBundle(const std::wstring& path, int* count);

// <cache>\lock, held open without delete sharing for the helper's life: the sweep's "owner alive" test.
HANDLE HoldLock(const std::wstring& cache);
// Deletes sibling instance folders (<pid>-<hex>) whose lock is free. Never follows reparse points.
void SweepDeadInstances(const std::wstring& cache);
bool DeleteTree(const std::wstring& dir);

// Runs a program with stdout and stderr captured, in its own kill-on-close job (the whole process tree dies on
// timeout). exitCode is -1 on a timeout.
struct RunResult {
    bool started = false, timedOut = false;
    int exitCode = -1;
    std::string out, err;
    std::string why;  // start failure
};
RunResult Run(const std::vector<std::wstring>& argv, const std::wstring& cwd, uint32_t timeoutMs, size_t maxOut, size_t maxErr);
}  // namespace holo::sandbox
