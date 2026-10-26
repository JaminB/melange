#pragma once
// The media pack and the yt-dlp lane (Documents\Melange\media). Written as a contract by Melange 0.12.0 C0; C4a
// implements it (pack.cpp, pack_logic.h, pack_fetch.cpp, the streaming extractor, ytdlp_lane.cpp) and C4b uses it to
// launch the helper and to answer wum.media.pack/install.
#include <cstdint>
#include <string>
#include <vector>

namespace melange::media::pack {
enum class State : uint8_t { Missing, Downloading, Verifying, Installing, Ready, Updating, Error, Unsupported, Disabled };
const char* StateName(State s);  // "missing", "downloading", ... as wum.media.pack().state

struct Info {
    State state = State::Missing;
    std::string version;            // the pack version compiled into this melange.asi (pack_manifest.h)
    double progress = 0;            // 0..1 while downloading/verifying/installing
    uint64_t bytes = 0, total = 0;  // while downloading
    uint32_t downloadMB = 0, diskMB = 0;  // from the compiled manifest, for the consent text
    std::string ytdlp;              // installed lane version, "" if none
    std::string error;              // State::Error: "hash" | "disk-space" | "network" | "cancelled" | "blocked" | ...
    std::wstring folder;            // the pack folder (for the Melange/Media panel)
};
Info Get();  // any thread (a copy)

// Starts the install on the media worker (pack first, then the lane). The caller (wum.media.install) has already
// checked the gesture; `modId` is logged ("[media] pack install requested by <id> (user click)"). False with a reason:
// "busy" (an install runs), "ready", "disabled", "unsupported-os", "blocked" ([Media] AllowDownload=0).
bool Install(const char* modId, std::string* reason);
void CancelInstall();

// Paths of a ready pack and lane, for the helper launch. False unless State::Ready.
struct Paths {
    std::wstring dir;        // Documents\Melange\media\<ver>
    std::wstring helperExe;  // <dir>\holo-media.exe
    std::wstring libmpv;     // <dir>\libmpv-2.dll, or [Media] CustomLibmpvPath with AllowCustomLibmpv=1
    std::wstring deno;       // <dir>\deno.exe
    std::wstring ytdlp;      // Documents\Melange\media\ytdlp\<ver>\yt-dlp.exe
    bool customLibmpv = false;
};
bool ReadyPaths(Paths* out);

// Before every helper launch (media worker thread, never the game thread: it hashes ~100 MB and may fetch a CRL).
// Opens holo-media.exe, libmpv-2.dll (unless custom), deno.exe and yt-dlp.exe with FILE_SHARE_READ only, re-hashes
// each against its manifest, and checks holo-media.exe's Authenticode publisher against melange.asi's (skipped and
// logged on an unsigned developer build). The handles stay open (no write, no delete) until ReleaseHeld.
struct Held {
    std::vector<void*> handles;  // HANDLEs
};
bool VerifyForLaunch(const Paths& p, Held* out, std::string* err);
void ReleaseHeld(Held* h);

// The yt-dlp lane: a check at most once per 24 h ([Media] YtDlpUpdate=1), or now with `force` (after a yt-dlp
// extraction or signature error). Asynchronous; the result shows in Get().ytdlp and a "pack" media event.
void CheckYtdlpLane(bool force);
}  // namespace melange::media::pack
