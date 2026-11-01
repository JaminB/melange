// Melange.ini [Spotify] (config_keys.h).
#include "spotify/config_keys.h"

#include <algorithm>
#include <cstdio>

#include "core/config.h"
#include "core/config_schema.h"
#include "core/log.h"
#include "spotify/spotify_logic.h"

namespace melange::spotify::cfg {
namespace {
logic::WarnOnce g_warned;  // a re-read (every 2 s while a mod uses wum.spotify) warns only about a new value

int IntKey(const char* section, const char* key, int def, int lo, int hi) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", def);
    config::EnsureKey(section, key, buf);
    const int v = config::GetInt(section, key, def);
    const int c = std::clamp(v, lo, hi);
    if (c != v && g_warned.First(key, std::to_string(v))) LOG_WARN("[%s] %s=%d is out of range (%d..%d), using %d", section, key, v, lo, hi, c);
    return c;
}
std::string StrKey(const char* section, const char* key, const char* def) {
    config::EnsureKey(section, key, def);
    return config::GetString(section, key, def);
}
std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
}  // namespace

Spotify ReadSpotify() {
    constexpr const char* S = "Spotify";
    Spotify s;
    const std::string id = Trim(StrKey(S, "ClientId", ""));
    config::schema::MarkLive(S, "ClientId");
    if (id.empty() || logic::ValidClientId(id)) {
        s.clientId = id;
    } else if (g_warned.First("ClientId", id)) {
        LOG_WARN("[Spotify] ClientId is not a 32-character Spotify Client ID; it is ignored");
    }
    s.redirectPort = IntKey(S, "RedirectPort", 8780, 1024, 65535);
    s.requestsPerMinute = IntKey(S, "RequestsPerMinute", 90, 10, 600);
    config::EnsureKey(S, "ReturnToGame", "1");
    s.returnToGame = config::GetBool(S, "ReturnToGame", true);
    return s;
}
}  // namespace melange::spotify::cfg
