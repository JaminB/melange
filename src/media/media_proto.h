#pragma once
// The media helper protocol (melange.asi <-> holo-media.exe): message names, versions, caps and timings.
// Shared by the x86 core and the x64 helper, so it is plain C++ with no Windows headers. Frozen for Melange 0.12.0:
// a change needs both sides and a new kProto (docs/media-helper.md).
#include <cstddef>
#include <cstdint>

namespace melange::media::proto {
inline constexpr int kProto = 1;

// ---- pipe (newline-delimited JSON, one object per line, "t" names the message)
inline constexpr size_t kMaxLine = 64 * 1024;  // bytes per line without the '\n'; longer lines are dropped whole
inline constexpr int kMaxDepth = 8;            // nesting of a decoded line
inline constexpr size_t kMaxNodes = 8192;      // values in a decoded line
inline constexpr size_t kMaxEventQueue = 256;  // helper->core messages queued for the main thread (props coalesce)

// ---- strings (bytes, UTF-8)
inline constexpr size_t kMaxUrlBytes = 4096;     // wum.media.classify input, canonical YouTube URLs are far shorter
inline constexpr size_t kMaxMagnetBytes = 1024;  // a canonical magnet (fits the peer bus string cap)
inline constexpr size_t kMaxNameBytes = 200;     // magnet dn
inline constexpr size_t kMaxTrackers = 8;        // magnet tr
inline constexpr size_t kMaxTrackerBytes = 256;
inline constexpr size_t kMaxTitleBytes = 512;    // media-title, resolve titles, track titles
inline constexpr size_t kMaxMessageBytes = 512;  // error messages
inline constexpr size_t kMaxPathBytes = 1024;    // torrent file paths
inline constexpr size_t kMaxShortBytes = 64;     // property and event names, codes, versions, language codes

// ---- per-mod limits (wum.media)
inline constexpr int kMaxPlayersPerMod = 1;
inline constexpr int kMaxTorrentsPerMod = 4;
inline constexpr int kMaxResolvesPerMod = 4;
inline constexpr size_t kMaxResolveItems = 200;
inline constexpr size_t kMaxTracks = 64;          // per kind in track-list
inline constexpr size_t kMaxTorrentFiles = 1000;  // bt.files entries
inline constexpr uint32_t kResolveTimeoutMs = 30000;

// ---- frames (see shm_logic.h)
inline constexpr uint32_t kFrameMaxW = 1280;
inline constexpr uint32_t kFrameMaxH = 720;

// ---- watchdog and rates (Critique T1)
inline constexpr uint32_t kPingIntervalMs = 2000;   // writer thread
inline constexpr uint32_t kHangKillMs = 6000;       // no pong for this long on the reader's clock, process alive
inline constexpr uint32_t kRestartMinGapMs = 60000; // at most one automatic restart per minute, then "dead"
inline constexpr uint32_t kTimePosHz = 10;          // helper throttle for time-pos / audio-pts
inline constexpr uint32_t kBtStatusHz = 1;

// ---- quality caps (per-player height cap, clamped to [Media] MaxHeight)
inline constexpr int kHeights[] = {360, 480, 720, 1080};

// ---- message names ("t")
namespace msg {
// core -> helper
inline constexpr const char kHello[] = "hello";
inline constexpr const char kOpen[] = "open";
inline constexpr const char kSet[] = "set";
inline constexpr const char kSeek[] = "seek";
inline constexpr const char kSize[] = "size";
inline constexpr const char kClose[] = "close";
inline constexpr const char kResolve[] = "resolve";
inline constexpr const char kBtAdd[] = "bt.add";
inline constexpr const char kBtSelect[] = "bt.select";
inline constexpr const char kBtRemove[] = "bt.remove";
inline constexpr const char kQuit[] = "quit";
inline constexpr const char kPing[] = "ping";
// helper -> core ("hello" is used in both directions)
inline constexpr const char kProp[] = "prop";
inline constexpr const char kEvent[] = "event";
inline constexpr const char kError[] = "error";
inline constexpr const char kResolved[] = "resolved";
inline constexpr const char kBtFiles[] = "bt.files";
inline constexpr const char kBtStatus[] = "bt.status";
inline constexpr const char kBtRemoved[] = "bt.removed";
inline constexpr const char kPong[] = "pong";
}  // namespace msg

// ---- properties the helper reports (prop.name); mpv's own names
namespace prop {
inline constexpr const char kTimePos[] = "time-pos";          // number, with qpc
inline constexpr const char kAudioPts[] = "audio-pts";        // number, with qpc; preferred when the item has audio
inline constexpr const char kPause[] = "pause";               // bool
inline constexpr const char kPausedForCache[] = "paused-for-cache";  // bool
inline constexpr const char kDuration[] = "duration";         // number
inline constexpr const char kTrackList[] = "track-list";      // tracks
inline constexpr const char kCacheTime[] = "demuxer-cache-time";  // number (seconds of media buffered)
inline constexpr const char kEofReached[] = "eof-reached";    // bool
inline constexpr const char kVideoParams[] = "video-params";  // video {w, h, aspect}
inline constexpr const char kMediaTitle[] = "media-title";    // string
inline constexpr const char kSpeed[] = "speed";               // number
}  // namespace prop

// ---- properties core may set (set.prop)
namespace setprop {
inline constexpr const char kPause[] = "pause";    // bool
inline constexpr const char kVolume[] = "volume";  // number 0..100
inline constexpr const char kMute[] = "mute";      // bool
inline constexpr const char kSpeed[] = "speed";    // number 0.25..4
inline constexpr const char kAid[] = "aid";        // track id, or false = off
inline constexpr const char kSid[] = "sid";        // track id, or false = off
}  // namespace setprop

// ---- event.name and event.reason
namespace event {
inline constexpr const char kFileLoaded[] = "file-loaded";
inline constexpr const char kEndFile[] = "end-file";  // reason: "eof" | "error" | "stop"
}  // namespace event

// ---- error.code (helper -> core); resolved.error uses the resolve codes
namespace err {
inline constexpr const char kLoad[] = "load";            // mpv could not open the item
inline constexpr const char kShm[] = "shm";              // the frame section could not be mapped
inline constexpr const char kProto[] = "proto";          // a core message the helper could not use
inline constexpr const char kTorrent[] = "torrent";      // libtorrent add/select failed
inline constexpr const char kDisk[] = "disk";            // cache folder full or not writable
inline constexpr const char kInternal[] = "internal";
// resolve
inline constexpr const char kYtdlp[] = "ytdlp";      // yt-dlp failed (extraction or signature error: core checks the lane)
inline constexpr const char kNetwork[] = "network";
inline constexpr const char kPrivate[] = "private";  // private, removed or age-gated
inline constexpr const char kTimeout[] = "timeout";
}  // namespace err

// ---- helper command line (core builds it; the helper rejects anything else)
namespace arg {
inline constexpr const wchar_t kPipe[] = L"--pipe";      // \\.\pipe\melange-holo-<pid>-<rand64> (core is the server)
inline constexpr const wchar_t kParent[] = L"--parent";  // the game's pid: the helper exits when it dies
inline constexpr const wchar_t kCache[] = L"--cache";    // this instance's writable folder (LocalLow\...\<pid>-<rand>)
inline constexpr const wchar_t kYtdlp[] = L"--ytdlp";    // full path of the lane's yt-dlp.exe
inline constexpr const wchar_t kDeno[] = L"--deno";      // full path of the pack's deno.exe
inline constexpr const wchar_t kLibmpv[] = L"--libmpv";  // full path of libmpv-2.dll (the pack's, or AllowCustomLibmpv)
inline constexpr const wchar_t kSweep[] = L"--sweep";    // first launch in this game process: sweep dead instances
inline constexpr const wchar_t kSelftest[] = L"--selftest";  // CI only: play av://lavfi:testsrc into a private section
}  // namespace arg
}  // namespace melange::media::proto
