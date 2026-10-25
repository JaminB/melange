#include "bt_bridge.h"

#include "log.h"
#include "media/url_logic.h"

#if HOLO_HAVE_MPV
#include <mpv/client.h>
#include <mpv/stream_cb.h>
#endif

namespace holo {
namespace url = melange::media::url;

void BtBridge::Service() {
    std::deque<std::shared_ptr<Request>> todo;
    {
        std::lock_guard lk(mu_);
        todo.swap(pending_);
    }
    if (todo.empty()) return;
    for (auto& r : todo) {
        if (engine_) r->stream = engine_->Open(r->ih, r->index, &r->err);
        else r->err = "no torrent engine";
        std::lock_guard lk(mu_);
        r->done = true;
    }
    cv_.notify_all();
}

void BtBridge::Cancel(const std::string& ih) {
    std::lock_guard lk(mu_);
    for (Cookie* c : live_)
        if (c->ih == ih && c->s) c->s->Cancel();
}

void BtBridge::Shutdown() {
    std::lock_guard lk(mu_);
    shutdown_ = true;
    for (Cookie* c : live_)
        if (c->s) c->s->Cancel();
    for (auto& r : pending_) r->done = true;
    pending_.clear();
    cv_.notify_all();
}

void BtBridge::Forget(Cookie* c) {
    std::lock_guard lk(mu_);
    live_.erase(c);
}

int BtBridge::Open(void* self, char* uri, void* info) { return static_cast<BtBridge*>(self)->OpenOn(uri ? uri : "", info); }

int BtBridge::OpenOn(const std::string& uri, void* info) {
#if HOLO_HAVE_MPV
    // holobt://<infohash>/<index>, exactly as bt::StreamUrl builds it.
    const std::string pre = std::string(bt::kStreamProtocol) + "://";
    if (uri.size() > 128 || uri.rfind(pre, 0) != 0) return MPV_ERROR_LOADING_FAILED;
    const size_t slash = uri.find('/', pre.size());
    if (slash == std::string::npos) return MPV_ERROR_LOADING_FAILED;
    auto req = std::make_shared<Request>();
    req->ih = uri.substr(pre.size(), slash - pre.size());
    const std::string idx = uri.substr(slash + 1);
    if (!url::ValidInfohash(req->ih) || idx.empty() || idx.size() > 4) return MPV_ERROR_LOADING_FAILED;
    for (char c : idx)
        if (c < '0' || c > '9') return MPV_ERROR_LOADING_FAILED;
    req->index = std::stoi(idx);
    {
        std::unique_lock lk(mu_);
        if (shutdown_) return MPV_ERROR_LOADING_FAILED;
        pending_.push_back(req);
        SetEvent(wake_);
        if (!cv_.wait_for(lk, std::chrono::seconds(15), [&] { return req->done; }) || !req->stream) {
            HLOG("bt: stream %s/%d not opened: %s", req->ih.c_str(), req->index, req->done ? req->err.c_str() : "timed out");
            return MPV_ERROR_LOADING_FAILED;
        }
    }
    auto* c = new Cookie{this, req->ih, std::move(req->stream)};
    {
        std::lock_guard lk(mu_);
        live_.insert(c);
    }
    auto* i = static_cast<mpv_stream_cb_info*>(info);
    i->cookie = c;
    i->read_fn = [](void* ck, char* buf, uint64_t n) -> int64_t { return static_cast<Cookie*>(ck)->s->Read(buf, n); };
    i->seek_fn = [](void* ck, int64_t off) -> int64_t {
        return static_cast<Cookie*>(ck)->s->Seek(off) ? off : static_cast<int64_t>(MPV_ERROR_GENERIC);
    };
    i->size_fn = [](void* ck) -> int64_t {
        const int64_t n = static_cast<Cookie*>(ck)->s->Size();
        return n >= 0 ? n : static_cast<int64_t>(MPV_ERROR_UNSUPPORTED);
    };
    i->close_fn = [](void* ck) {
        auto* c = static_cast<Cookie*>(ck);
        c->bridge->Forget(c);
        delete c;
    };
    i->cancel_fn = [](void* ck) { static_cast<Cookie*>(ck)->s->Cancel(); };
    return 0;
#else
    (void)uri;
    (void)info;
    return -1;
#endif
}
}  // namespace holo
