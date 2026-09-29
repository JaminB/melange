#pragma once
#include <cstdint>
#include <string>

#include "melange/wormsign.h"

// The replay library -- indexing, retention and pin state behind melange::wormsign::Library()/Pin() (defined in
// library.cpp, since those two are declared directly in melange/wormsign.h, not under this namespace). This
// header is the internal surface the recorder and the overlay/Oasis providers use.
namespace melange::wormsign::library {
// <Documents>\Melange\replays -- created on first use. Empty string if Documents could not be resolved.
std::wstring ReplaysDir();

// Test-only: redirects ReplaysDir() to `dir` (created if missing) instead of the real Documents folder, so the
// offline self-test can exercise Rescan()/retention/export against a throwaway directory. Empty restores the
// real path.
void SetReplaysDirForTests(const std::wstring& dir);

// Called once a recording's file is closed (normally or by a crash-safe Abandon). Indexes it and enforces
// retention (KeepMatches / MaxMB from ini; pinned and flagged files are exempt from both).
void OnRecordingClosed(const std::wstring& path, bool complete);

// Rescans ReplaysDir() from disk (headers + trailers, tail-recovery for incomplete files). Safe to call anytime;
// Install() calls it once at startup so Library() has data before the first match closes.
void Rescan();

void Configure(int keepMatches, uint32_t maxMB);

// Copies `path` to `outPath` for sharing: SteamID-shaped numbers and IPv4 addresses in the file's JSON
// (HEAD/SETP/NOTE) chunks are salted-hashed, and the current Windows user name is replaced with %USERNAME%,
// exactly as tools/redact.h does for "Save logs as...". Binary chunks (INPT, RMTI, DISP, TICK, DETL, ENGV) carry
// no identity data and pass through unchanged. The local, non-exported file on disk is never touched by this.
bool ExportRedacted(const std::wstring& path, const std::wstring& outPath, std::string* error);
}  // namespace melange::wormsign::library
