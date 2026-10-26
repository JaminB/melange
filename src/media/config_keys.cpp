// Melange.ini keys for Holo's core features (config_keys.h).
#include "media/config_keys.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "core/config.h"
#include "core/log.h"

namespace melange::media::cfg {
namespace {
int IntKey(const char* section, const char* key, int def, int lo, int hi) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", def);
    config::EnsureKey(section, key, buf);
    const int v = config::GetInt(section, key, def);
    const int c = std::clamp(v, lo, hi);
    if (c != v) LOG_WARN("[%s] %s=%d is out of range (%d..%d), using %d", section, key, v, lo, hi, c);
    return c;
}
bool BoolKey(const char* section, const char* key, bool def) { return IntKey(section, key, def ? 1 : 0, 0, 1) != 0; }
std::string StrKey(const char* section, const char* key, const char* def) {
    config::EnsureKey(section, key, def);
    return config::GetString(section, key, def);
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
}  // namespace

Media ReadMedia() {
    constexpr const char* S = "Media";
    Media m;
    m.allowDownload = BoolKey(S, "AllowDownload", true);
    m.packUrl = StrKey(S, "PackUrl", "");
    m.matchDownloadKBps = IntKey(S, "MatchDownloadKBps", 2048, 64, 1048576);
    const int h = IntKey(S, "MaxHeight", 1080, 360, 1080);
    m.maxHeight = h >= 1080 ? 1080 : h >= 720 ? 720 : h >= 480 ? 480 : 360;
    m.audioDelayMs = IntKey(S, "AudioDelayMs", 25, -500, 500);
    m.hwdec = StrKey(S, "Hwdec", "auto-copy");
    m.torrentCacheMB = IntKey(S, "TorrentCacheMB", 8192, 256, 262144);
    m.torrentUploadKBps = IntKey(S, "TorrentUploadKBps", 1024, 1, 1048576);
    m.torrentUpnp = BoolKey(S, "TorrentUPnP", false);
    m.torrentPort = IntKey(S, "TorrentPort", 0, 0, 65535);
    if (m.torrentPort != 0 && m.torrentPort < 1024) m.torrentPort = 0;
    m.ytdlpUpdate = BoolKey(S, "YtDlpUpdate", true);
    m.helperLowIntegrity = BoolKey(S, "HelperLowIntegrity", true);
    m.cacheDir = Wide(StrKey(S, "CacheDir", ""));
    m.allowCustomLibmpv = BoolKey(S, "AllowCustomLibmpv", false);
    m.customLibmpv = Wide(StrKey(S, "CustomLibmpvPath", ""));
    return m;
}

bool AllowMusicSuppress() { return BoolKey("Audio", "AllowMusicSuppress", true); }
bool AllowModInput() { return BoolKey("Input", "AllowModInput", true); }
}  // namespace melange::media::cfg
