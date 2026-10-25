#pragma once
// Melange.ini keys added in Melange 0.12.0 for Holo's core features, in one place: [Media], [PeerBus],
// [Audio] AllowMusicSuppress and [Input] AllowModInput. Each reader writes its keys' defaults the first time (like
// Module::Int), so a fresh ini documents them. The modules "Media" (media.cpp) and "PeerBus" (peerbus.cpp) own their
// sections' Enabled key through modules::InstallAll; nothing here writes Enabled. Main thread or install time.
#include <string>

namespace melange::media::cfg {
struct Media {
    bool allowDownload = true;      // AllowDownload: 0 blocks pack installs and yt-dlp lane updates
    std::string packUrl;            // PackUrl: "" = the compiled manifest; else file:/// (tests) or the allowed https prefix
    int matchDownloadKBps = 2048;   // MatchDownloadKBps: download cap during a match (64..1048576)
    int maxHeight = 1080;           // MaxHeight: hard cap on the per-player quality (360|480|720|1080)
    int audioDelayMs = 25;          // AudioDelayMs: mpv audio-delay (-500..500)
    std::string hwdec = "auto-copy";  // Hwdec
    int torrentCacheMB = 8192;      // TorrentCacheMB (256..262144)
    int torrentUploadKBps = 1024;   // TorrentUploadKBps: seeding cap (1..1048576)
    bool torrentUpnp = false;       // TorrentUPnP: UPnP and NAT-PMP
    int torrentPort = 0;            // TorrentPort: 0 = random (else 1024..65535)
    bool ytdlpUpdate = true;        // YtDlpUpdate: the lane check (at most once per 24 h)
    bool helperLowIntegrity = true; // HelperLowIntegrity
    std::wstring cacheDir;          // CacheDir: "" = %USERPROFILE%\AppData\LocalLow\Melange\media-cache
    bool allowCustomLibmpv = false; // AllowCustomLibmpv (LGPL relink right)
    std::wstring customLibmpv;      // CustomLibmpvPath: the user's libmpv-2.dll when AllowCustomLibmpv=1
};

// [Media]: defaults written, values range-checked and clamped (an out-of-range value logs once and uses the clamp).
Media ReadMedia();
// [PeerBus] has only Enabled (the module's); [Audio] AllowMusicSuppress (default 1); [Input] AllowModInput (default 1).
bool AllowMusicSuppress();
bool AllowModInput();
// The allowed prefix for the pack and the yt-dlp lane (https only, GitHub releases of JaminB/melange).
inline constexpr const char kAllowedUrlPrefix[] = "https://github.com/JaminB/melange/releases/download/media-";
}  // namespace melange::media::cfg
