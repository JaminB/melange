#pragma once
// The helper's main loop: the core's messages, the players' mpv events, the torrent engine's alerts and the holobt
// stream opens, all on one thread (bt.h's rule), woken by the pipe, by mpv and at least every 50 ms.
#include <windows.h>

#include <map>
#include <memory>
#include <string>

#include "args.h"
#include "bt/bt.h"
#include "bt_bridge.h"
#include "pipe_client.h"
#include "player.h"
#include "resolve.h"

namespace holo {
class App {
public:
    explicit App(const Args& a);
    // Returns the process exit code (0 on quit, 1 when the game or the pipe went away).
    int Run();

    // The mpv and ffmpeg versions of the loaded libmpv, from a throwaway core with the same hardened options.
    static void ProbeVersions(const std::string& mpvHome, std::string* mpv, std::string* ffmpeg);

private:
    void Handle(melange::media::pipe::Message& m);
    void OnOpen(const melange::media::pipe::OpenCmd& c);
    void OnBtAdd(const melange::media::pipe::BtAddCmd& c);
    void PollEngine();
    void Error(uint32_t id, uint32_t gen, const std::string& ih, const char* code, const std::string& message);
    void Shutdown();

    Args args_;
    HANDLE wake_ = nullptr;    // mpv wakeups and holobt opens
    HANDLE parent_ = nullptr;  // the game: the helper exits when it ends
    PipeClient pipe_;
    PlayerEnv env_;
    std::map<uint32_t, std::unique_ptr<Player>> players_;
    std::unique_ptr<bt::Engine> engine_;
    std::unique_ptr<BtBridge> bridge_;
    std::unique_ptr<Resolver> resolver_;
    std::string libmpvError_;
    bool quit_ = false;
};
}  // namespace holo
