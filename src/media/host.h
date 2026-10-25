#pragma once
// The media host as wum.media sees it (wum_media.cpp): players, resolves and torrents, scoped to the mod that made
// them. Main thread only. Implemented by media.cpp; sandbox_selftest fakes it (tests/sandbox_fakes_media.inc), so the
// Lua binding is tested without a helper. Results reach Lua through the Sink that wum_media.cpp installs.
#include <cstdint>
#include <string>
#include <vector>

#include "tools/json_read.h"

namespace melange::media::host {
// true, or false and "disabled" | "unsupported-os" | "unavailable".
bool Available(std::string* reason);
// The mod uses wum.media: it receives the throttled "pack" events from now on.
void Touch(const std::string& mod);

struct OpenSpec {
    std::string url;       // a canonical YouTube video URL (url::Classify), or empty
    bool live = false;     // classify said /live/
    uint32_t torrent = 0;  // this mod's torrent handle id, with `file`
    int file = -1;
    double start = 0;
    bool paused = true, mute = false;
    int volume = 70;
    int quality = 720;     // 360 | 480 | 720 | 1080 before the [Media] MaxHeight clamp
    double speed = 1;
    std::string alang, slang;  // url::ValidLang or empty
};
// The new player's id, or 0 and "busy" | "pack" | "unsupported-url" | "cache-cap" | "helper" | "closed" (the torrent).
uint32_t Open(const std::string& mod, const OpenSpec& spec, std::string* reason);

// Every call below is false when `id` is not an open player of `mod` (wum.media then answers nil, "closed").
bool SetPaused(const std::string& mod, uint32_t id, bool paused);
bool Seek(const std::string& mod, uint32_t id, double t, const std::string& mode);  // exact | keyframes | relative
bool SetSpeed(const std::string& mod, uint32_t id, double speed);    // clamped 0.25..4
bool SetVolume(const std::string& mod, uint32_t id, double volume);  // clamped 0..100
bool SetMute(const std::string& mod, uint32_t id, bool mute);
bool SetTrack(const std::string& mod, uint32_t id, bool audio, int track);  // track 0 = off
bool SetQuality(const std::string& mod, uint32_t id, int height);
bool SetSize(const std::string& mod, uint32_t id, int w, int h);
struct FollowState {
    double e = 0;
    std::string i;
    bool play = false;
    double p = 0, r = 1, a = 0;
};
bool Follow(const std::string& mod, uint32_t id, const FollowState* st, uint64_t hostId);  // st null: stop following

struct Track {
    int id = 0;
    std::string lang, title;
    bool selected = false;
};
struct PlayerState {
    double time = 0, duration = 0, cacheAhead = 0, speed = 1, aspect = 0;
    bool paused = true, buffering = false, eof = false, live = false;
    std::string title;
    int w = 0, h = 0;
    std::vector<Track> audio, sub;
    bool following = false, hasDrift = false;
    double drift = 0;
    const char* syncAction = "none";
    const char* helper = "starting";  // starting | ready | restarting | dead
};
bool State(const std::string& mod, uint32_t id, PlayerState* out);
bool Close(const std::string& mod, uint32_t id);

// A playlist or video listing through yt-dlp. The request id, or 0 and "busy" (4 in flight) | "pack" | "helper".
// The answer comes through Sink::Callback(mod, token, {items, title} | {error, message}, done = true).
uint32_t Resolve(const std::string& mod, const std::string& canonicalUrl, uint32_t token, std::string* reason);
void CancelResolve(const std::string& mod, uint32_t rid);

struct File {
    int index = 0;
    std::string path;
    double size = 0;
    bool video = false;
};
struct TorrentStatus {
    double down = 0, up = 0, progress = 0, have = 0;
    int peers = 0;
    bool metadata = false;
};
// A torrent from a canonical magnet. Its id, or 0 and "busy" (4 open) | "pack" | "helper" | "first-use-in-match".
// The file list comes through Sink::Callback(mod, token, {files, name} | {error}, done = false) and a "torrent" event.
uint32_t AddTorrent(const std::string& mod, const std::string& magnet, uint32_t token, bool inMatch, std::string* reason);
bool TorrentFiles(const std::string& mod, uint32_t id, std::vector<File>* out, bool* have);
bool TorrentStatusOf(const std::string& mod, uint32_t id, TorrentStatus* out);
bool CloseTorrent(const std::string& mod, uint32_t id);

// Delivery to Lua, installed once by wum_media.cpp. Both run on the main thread from the media frame pump.
class Sink {
public:
    virtual ~Sink() = default;
    // A melange.media event for `mod` only ({type = ..., ...}).
    virtual void Event(const std::string& mod, const json::Value& payload) = 0;
    // The function registered with `token` (a resolve's or a torrent's); `done`: it is not called again.
    virtual void Callback(const std::string& mod, uint32_t token, const json::Value& arg, bool done) = 0;
};
void SetSink(Sink* sink);
}  // namespace melange::media::host
