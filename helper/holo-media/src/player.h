#pragma once
// One player = one mpv core with the software render API, a render thread and the player's frame section.
// The main thread owns the mpv handle's events (wakeup callback -> the main loop's event) and every command, all
// sent asynchronously so the main loop never waits on mpv. The render thread waits for mpv's update callback, renders
// bgr0 into the triple buffer's free slot (shm::Writer) at the fitted size and publishes it. Closing hands the whole
// player to a teardown thread (render thread joined, render context freed, mpv destroyed) that the main loop never
// waits for.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "media/pipe_codec.h"
#include "media/shm_logic.h"

struct mpv_handle;
struct mpv_render_context;

namespace holo {
class BtBridge;

// What every player shares: the core's settings and paths, and where its messages go.
struct PlayerEnv {
    std::function<void(const melange::media::pipe::Message&, bool droppable)> send;
    HANDLE wake = nullptr;         // the main loop's event (mpv's wakeup callback sets it)
    std::string mpvHome;           // <cache>\mpvhome, UTF-8
    std::string ytdlp, deno;       // full paths, UTF-8
    std::string caFile;            // <cache>\home\roots.pem (tls-ca-file), or empty
    melange::media::pipe::HelloCmd hello;
    BtBridge* bt = nullptr;        // holobt:// streams; nullptr = none
};

// One load of a player (an `open`, or the selftest's own item). url is already canonical (or holobt://, or the
// selftest's lavfi source).
struct LoadSpec {
    uint32_t gen = 0;
    std::string shm;
    std::string url;
    bool ytdl = false;
    double start = 0;
    bool paused = true, mute = false;
    int volume = 70;
    double speed = 1;
    int height = 720;
    std::string alang, slang;
    std::string ih;  // torrent items: the infohash (for bt.remove)
};

class Player {
public:
    static std::unique_ptr<Player> Create(uint32_t id, const PlayerEnv& env, std::string* why);
    ~Player();
    uint32_t Id() const { return id_; }
    const std::string& Ih() const { return ih_; }

    // False with error code/message ("shm" when the section cannot be mapped).
    bool Load(const LoadSpec& s, std::string* code, std::string* why);
    void Set(const melange::media::pipe::SetCmd& c);
    void Seek(const melange::media::pipe::SeekCmd& c);
    void Size(int w, int h);
    void Stop();  // the item's stream goes away (bt.remove): end the file, keep the player

    // Main thread: drains mpv's events into messages, and sends throttled props whose interval passed.
    void Drain();
    void Tick();

    // Hands the player to a detached teardown thread. Safe at any time; the object is gone afterwards.
    static void Close(std::unique_ptr<Player> p);
    static bool WaitTeardowns(uint32_t ms);  // quit path: true when every closed player is fully destroyed

    // selftest
    uint64_t FramesPublished() const { return frames_.load(); }
    std::string GetString(const char* prop);

private:
    explicit Player(uint32_t id, const PlayerEnv& env) : id_(id), env_(env) {}
    bool Init(std::string* why);
    bool MapSection(const std::string& name, std::string* why);
    void UnmapSection();
    void RenderLoop();
    void OnEvent(void* ev);
    void OnProperty(void* ev);
    void Emit(melange::media::pipe::PropEvt& e, int slot);
    void Teardown();

    const uint32_t id_;
    PlayerEnv env_;
    mpv_handle* h_ = nullptr;
    mpv_render_context* rc_ = nullptr;
    std::string ih_;

    // section (render thread and main thread, under secMu_)
    std::mutex secMu_;
    std::string secName_;
    HANDLE secMap_ = nullptr;
    void* secView_ = nullptr;
    std::unique_ptr<melange::media::shm::Writer> writer_;

    // render thread
    std::thread render_;
    HANDLE renderWake_ = nullptr;
    std::atomic<bool> stop_{false}, force_{false};
    std::atomic<int> boxW_{0}, boxH_{0}, vidW_{0}, vidH_{0};
    std::atomic<double> aspect_{0};
    std::atomic<uint64_t> frames_{0};

    // load generations: an open's gen is taken by the next start-file; end-file reports the gen of its own entry
    std::deque<uint32_t> pendingGens_;
    std::map<int64_t, uint32_t> entryGen_;
    uint32_t activeGen_ = 0;
    uint32_t eofGen_ = 0;
    uint32_t playingGen_ = 0;  // activeGen_ + 1 once that load's first playback-restart was logged  // activeGen_ + 1 once that load's end was reported (0 = not yet)

    // throttled props (time-pos, audio-pts, demuxer-cache-time)
    struct Throttled {
        melange::media::pipe::PropEvt last;
        double sentMs = -1e9;
        bool pending = false;
    };
    Throttled thr_[3];
    int logLines_ = 0;

    static std::atomic<int> teardowns_;
};
}  // namespace holo
