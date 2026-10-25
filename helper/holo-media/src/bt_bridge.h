#pragma once
// mpv reads torrent files through the "holobt" stream protocol (mpv_stream_cb_add_ro), never a loopback server.
// mpv opens a stream on its own demuxer thread, but every bt::Engine call belongs to the main thread (bt.h), so the
// open callback queues a request, wakes the main loop, and waits for the main thread to call Engine::Open. Reads,
// seeks and cancels then go straight to the bt::Stream (bt.h allows Read and Cancel from other threads).
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "bt/bt.h"

namespace holo {
class BtBridge {
public:
    explicit BtBridge(HANDLE mainWake) : wake_(mainWake) {}
    void SetEngine(bt::Engine* e) { engine_ = e; }  // main thread
    // Main thread, every loop turn: answers the queued opens.
    void Service();
    // Any thread: unblocks every open stream of one torrent (bt.remove) or all of them (quit). Later opens fail.
    void Cancel(const std::string& ih);
    void Shutdown();
    // The stream_cb open callback's user data is the bridge.
    static int Open(void* self, char* uri, void* info);  // info: mpv_stream_cb_info*

    struct Request {
        std::string ih;
        int index = 0;
        std::unique_ptr<bt::Stream> stream;
        std::string err;
        bool done = false;
    };
    struct Cookie {
        BtBridge* bridge;
        std::string ih;
        std::unique_ptr<bt::Stream> s;
    };

private:
    int OpenOn(const std::string& uri, void* info);
    void Forget(Cookie* c);

    HANDLE wake_;
    bt::Engine* engine_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Request>> pending_;
    std::set<Cookie*> live_;
    bool shutdown_ = false;
};
}  // namespace holo
