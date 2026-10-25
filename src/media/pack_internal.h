#pragma once
// The media pack installer's disk and network steps (Melange 0.12.0, C4a), without the game: pack_fetch.cpp (resumable,
// hashed, throttled download), pack_extract.cpp (the streaming extractor, the install into place, verify, sweep and
// uninstall) and ytdlp_lane.cpp (the yt-dlp lane). pack.cpp adds the state machine, the worker thread, the config, the
// mutexes and the Authenticode check; media_selftest drives everything here with file:/// fixtures.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "media/pack_logic.h"

namespace melange::media::pack::core {
// Every step reports through this; all members optional. Called on the worker thread.
struct Hooks {
    const std::atomic<bool>* cancel = nullptr;
    std::function<uint32_t()> throttleKBps;                   // asked during downloads; 0 = no cap
    std::function<void(uint64_t done, uint64_t total)> progress;  // at most every 100 ms, and once at the end
    std::function<void(const char* phase)> phase;             // "downloading", "verifying", "installing"
    std::string userAgent = "Melange";
};

// ---------------------------------------------------------------- download (pack_fetch.cpp)
// Downloads `url` to `part`, hashing as it streams. An existing `part` shorter than `size` is hashed first and the
// download resumes after it (Range on https, a seek on file:///); a server that ignores the range restarts it. On a
// size or hash mismatch `part` is deleted and the error is "hash"; a dropped connection keeps `part` for the next try
// ("network"). Other errors: "cancelled", "disk" (cannot write). `detail` gets the human-readable reason.
bool FetchResumable(const std::string& url, const std::wstring& part, uint64_t size, const std::string& sha256,
                    const Hooks& h, std::string* error, std::string* detail);

// ---------------------------------------------------------------- the extractor (pack_extract.cpp)
// Peak-heap accounting of the extractor and the download: every buffer they allocate (and miniz's, through
// mz_zip_archive's allocator hooks) is counted here, so the self-test can hold them to kMaxHeapBytes (Critique M1).
inline constexpr size_t kMaxHeapBytes = 4u << 20;
inline constexpr size_t kIoBufferBytes = 1u << 20;
struct HeapCounter {
    std::atomic<int64_t> current{0}, peak{0};
    void Reset() {
        current = 0;
        peak = 0;
    }
};
HeapCounter& Heap();
void* CountedAlloc(size_t n);
void CountedFree(void* p);

// Extracts the pack zip into `dest` (created, must not exist): only the manifest's exact names, each streamed to disk
// through a 1 MiB buffer with a running SHA-256 and checked against its size and hash. Every file lands inside `dest`
// (the final path is checked). Errors: "hash" (an entry differs), "zip" (refused or unreadable), "disk", "cancelled".
bool ExtractPack(const std::wstring& zip, const logic::Manifest& m, const std::wstring& dest, const Hooks& h,
                 std::string* error, std::string* detail);

// Folder layout under Documents\Melange\media (`root`).
struct Layout {
    std::wstring root;
    std::wstring Dl() const { return root + L"\\dl"; }
    std::wstring PackDir(const std::string& ver) const;
    std::wstring LaneRoot() const { return root + L"\\ytdlp"; }
};

// The whole install of `m` from `url`: free-space check, download to dl\pack-<ver>-<sha8>.zip.part (resumed), verify,
// extract into <ver>.tmp, check the extracted pack.json, rename to <ver> (a stale <ver> is moved aside first), write
// installed.json, delete the zip and older pack versions. Errors as above plus "disk-space". The caller holds the
// install mutex.
bool InstallPack(const Layout& l, const logic::Manifest& m, const std::string& url, const Hooks& h, std::string* error,
                 std::string* detail);
// A cheap readiness check at start: every manifest file exists with its size (no hashing).
bool PackPresent(const Layout& l, const logic::Manifest& m);
// Re-hashes every file of the installed pack (after pack.json's write time changed).
bool VerifyPackFiles(const Layout& l, const logic::Manifest& m, std::string* detail);
// pack.json's last write time as recorded at install (installed.json), and now on disk. 0 when unknown.
uint64_t RecordedPackJsonTime(const Layout& l, const logic::Manifest& m);
uint64_t PackJsonTime(const Layout& l, const logic::Manifest& m);
// Writes installed.json with pack.json's current write time (after an install, or a full re-verify).
bool RecordInstalled(const Layout& l, const logic::Manifest& m);
// Start-of-session sweep (caller holds the install mutex): <x>.tmp folders, dl\*.part older than 7 days.
void Sweep(const Layout& l);
// Moves <ver> and the lane aside and deletes them. False ("in-use") when a file is open, e.g. a running helper.
bool Uninstall(const Layout& l, std::string* error);
// Recursive delete that never follows a junction or a symbolic link (it removes the link itself).
bool DeleteTree(const std::wstring& dir);
// Opens `path` for reading with FILE_SHARE_READ only (no writer, no delete) and hashes it through that handle. The
// handle stays open in *held (the caller closes it). The final path must lie inside `inside` when given.
bool OpenHashed(const std::wstring& path, const std::wstring& inside, uint64_t size, const std::string& sha256, HANDLE* held,
                std::string* detail);

// ---------------------------------------------------------------- the yt-dlp lane (ytdlp_lane.cpp)
struct LaneState {
    logic::Lane lane;          // installed: version, sha256, size, serial (serial 0 = none)
    int64_t lastCheck = 0;     // unix seconds of the last successful manifest fetch
    std::wstring exe;          // <LaneRoot>\<version>\yt-dlp.exe when installed
};
LaneState ReadLane(const Layout& l);
// One lane check: fetch the manifest from `laneUrl` (allowed prefix, or file:/// for a dev lane), decide against the
// installed serial, download and verify a new yt-dlp into <LaneRoot>\<ver>, record lane.json, delete older versions.
// `action` reports the decision ("install", "record", "current", "rollback", "conflict"). Errors: "network", "hash",
// "manifest", "rollback", "conflict", "disk", "cancelled". The caller holds the lane mutex.
bool UpdateLane(const Layout& l, const std::string& laneUrl, std::string_view prefix, const Hooks& h, LaneState* out,
                std::string* action, std::string* error, std::string* detail);

// ---------------------------------------------------------------- small helpers shared by the files above
std::wstring Widen(std::string_view s);
std::string Narrow(const std::wstring& w);
bool MakeDirs(const std::wstring& dir);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);
int64_t UnixNow();
}  // namespace melange::media::pack::core

// The Melange/Media overlay panel (media_panel.cpp) and what pack.cpp gives it.
namespace melange::media::pack::panel {
void Register();             // once, from the media worker (overlay::AddPanel is any-thread)
std::string RequestedBy();   // the mod whose click started the last install, "" if none this session
bool InUse();                // a helper launch holds pack files (VerifyForLaunch .. ReleaseHeld)
void Uninstall();            // queued on the media worker; the result shows in Get().state / error
}  // namespace melange::media::pack::panel
