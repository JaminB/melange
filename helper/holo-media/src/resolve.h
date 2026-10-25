#pragma once
// wum.media.resolve in the helper: one thread per request runs yt-dlp (the whitelisted flags of mpv_opts.h, then
// --flat-playlist -J -- <canonical url>) in its own job, and sends one `resolved` line. At most 8 run at once (the
// core allows 4 per mod); a request over that answers error "ytdlp" at once.
#include <atomic>
#include <functional>
#include <string>

#include "media/pipe_codec.h"

namespace holo {
class Resolver {
public:
    using Send = std::function<void(const melange::media::pipe::Message&)>;
    Resolver(std::wstring ytdlp, std::wstring deno, std::wstring cwd, Send send)
        : ytdlp_(std::move(ytdlp)), deno_(std::move(deno)), cwd_(std::move(cwd)), send_(std::move(send)) {}
    void Start(uint32_t rid, const std::string& canonicalUrl);
    int Running() const { return running_.load(); }

private:
    std::wstring ytdlp_, deno_, cwd_;
    Send send_;
    std::atomic<int> running_{0};
};
}  // namespace holo
