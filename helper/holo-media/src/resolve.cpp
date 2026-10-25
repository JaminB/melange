#include "resolve.h"

#include <thread>

#include "log.h"
#include "mpv_opts.h"
#include "resolve_logic.h"
#include "sandbox.h"
#include "util.h"

namespace holo {
namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;
namespace url = melange::media::url;

namespace {
constexpr int kMaxRunning = 8;
// Answer before the core's own 30 s timeout, so the mod sees "timeout" and not a dropped request.
constexpr uint32_t kTimeoutMs = proto::kResolveTimeoutMs - 2000;
constexpr size_t kMaxStdout = 48u << 20, kMaxStderr = 64u << 10;
}  // namespace

void Resolver::Start(uint32_t rid, const std::string& canonicalUrl) {
    pipe::ResolvedEvt r;
    r.rid = rid;
    url::Classified c;
    if (!url::Classify(canonicalUrl, &c) || c.kind != url::Kind::YouTube) {  // the decoder did this; again, before a process
        r.error = proto::err::kYtdlp;
        r.message = "not a YouTube link";
        send_(pipe::Message{r});
        return;
    }
    if (running_.load() >= kMaxRunning) {
        r.error = proto::err::kYtdlp;
        r.message = "too many resolves at once";
        send_(pipe::Message{r});
        return;
    }
    ++running_;
    std::thread([this, r, u = c.url]() mutable {
        const double t0 = QpcMs();
        const auto res = sandbox::Run(opts::ResolveArgv(ytdlp_, deno_, Wide(u)), cwd_, kTimeoutMs, kMaxStdout, kMaxStderr);
        if (!res.started) {
            r.error = proto::err::kYtdlp;
            r.message = url::CleanName(res.why, proto::kMaxMessageBytes);
        } else if (res.timedOut) {
            r.error = proto::err::kTimeout;
        } else if (res.exitCode == 0 && resolve::ParseOutput(res.out, &r)) {
            resolve::FitLine(&r);
        } else {
            r.items.clear();
            r.title.clear();
            r.error = res.exitCode == 0 ? std::string(proto::err::kYtdlp) : resolve::ErrorCode(res.err);
            r.message = resolve::ErrorMessage(res.err);
        }
        HLOG("resolve %u: %s, %zu item(s), %.0f ms, exit %d%s%s", r.rid, r.error.empty() ? "ok" : r.error.c_str(), r.items.size(),
             QpcMs() - t0, res.exitCode, r.message.empty() ? "" : ": ", r.message.c_str());
        send_(pipe::Message{r});
        --running_;
    }).detach();
}
}  // namespace holo
