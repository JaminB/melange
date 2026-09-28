// The `log` channel: every jlog record, filtered per client by level/category/text, with a backlog of the
// last 500 matching records on subscribe. Driven by jlog's own seq-indexed ring (core/jlog_ring.h behind
// jlog::Tail), polled once per frame -- cheap when nobody is subscribed (one atomic load) and O(new lines)
// otherwise. Also `log.sessions` and the `/logs/<session>/<file>` route for past sessions.
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "melange/jlog.h"
#include "melange/oasis.h"
#include "oasis/core/server.h"
#include "oasis/providers.h"
#include "oasis/streams/wire.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
namespace streams = melange::oasis::streams;

constexpr size_t kBacklogMax = 500;
constexpr size_t kPollMax = 8192;  // one frame's worth even at the busiest configured rate
constexpr size_t kSessionsMax = 20;

struct Sub {
    streams::LogFilter filter;
    uint64_t from = 0;  // records up to this seq went out as this client's backlog
};

ChannelId g_ch = 0;
std::mutex g_mx;                           // guards both of these together
uint64_t g_lastSeq = 0;                    // highest jlog seq already polled for live subscribers
std::unordered_map<int, Sub> g_subs;       // client -> filter and backlog cursor

// Sends this client up to the last 500 matches as backlog; the live poll skips what the backlog covered for
// this client only, so other subscribers never lose a record.
void OnSub(ChannelId, int client, std::string_view filterJson, bool subscribed, void*) {
    if (!subscribed) {
        std::lock_guard lk(g_mx);
        g_subs.erase(client);
        return;
    }
    streams::LogFilter f = streams::ParseLogFilter(filterJson);
    std::lock_guard lk(g_mx);
    std::vector<jlog::Line> lines;
    jlog::Tail(0, lines, 1u << 20);
    const uint64_t head = lines.empty() ? 0 : lines.back().seq;
    g_subs[client] = Sub{f, head};
    std::vector<const jlog::Line*> matched;
    for (auto it = lines.rbegin(); it != lines.rend() && matched.size() < kBacklogMax; ++it)
        if (streams::MatchesLog(f, *it)) matched.push_back(&*it);
    for (auto it = matched.rbegin(); it != matched.rend(); ++it) PublishTo(g_ch, client, streams::BuildLogPayload(**it));
}

void PollFrame() {
    if (!HasSubscribers(g_ch)) return;
    std::lock_guard lk(g_mx);
    std::vector<jlog::Line> lines;
    jlog::Tail(g_lastSeq, lines, kPollMax);
    if (lines.empty()) return;
    g_lastSeq = lines.back().seq;
    for (const auto& l : lines)
        for (const auto& [client, s] : g_subs)
            if (l.seq > s.from && streams::MatchesLog(s.filter, l)) PublishTo(g_ch, client, streams::BuildLogPayload(l));
}

std::wstring BaseName(const std::wstring& path) {
    const size_t i = path.find_last_of(L"\\/");
    return i == std::wstring::npos ? path : path.substr(i + 1);
}

// Past and current session folders, newest first: {id, files: [name], bytes}.
void SessionsRpc(const Call&, Result& r, void*) {
    jsonmini::Arr sessions;
    for (const std::wstring& dir : jlog::RecentSessionDirs(kSessionsMax)) {
        jsonmini::Arr files;
        uint64_t bytes = 0;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                files.Str(game::Narrow(fd.cFileName));
                bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        sessions.Raw(jsonmini::Obj().Str("id", game::Narrow(BaseName(dir))).Raw("files", files.End()).UInt("bytes", bytes).End());
    }
    r.json = sessions.End();
}

// /logs/<session>/<file>: the session must be one log.sessions lists and the file a plain name inside it.
bool RouteLogs(const core::Request& rq, core::Response* out, void*) {
    const std::string rel = rq.path.substr(6);
    const size_t slash = rel.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rel.size()) return false;
    const std::wstring session = game::Widen(rel.substr(0, slash)), file = game::Widen(rel.substr(slash + 1));
    if (file.find_first_of(L"/\\:") != std::wstring::npos || file == L"." || file == L"..") return false;
    for (const std::wstring& dir : jlog::RecentSessionDirs(kSessionsMax)) {
        if (BaseName(dir) != session) continue;
        const std::wstring full = dir + L"\\" + file;
        const DWORD attr = GetFileAttributesW(full.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
        out->status = 200;
        out->contentType = core::MimeType(rel);
        out->file = full;
        return true;
    }
    return false;
}

}  // namespace

void InstallLog() {
    ChannelOptions opt;
    opt.overflow = Overflow::DropOldest;
    opt.maxQueueKB = 64;
    g_ch = AddChannel("log", opt);
    if (!g_ch) return;
    OnSubscribe(g_ch, &OnSub, nullptr);
    events::Subscribe(events::Event::Frame, &PollFrame);
    AddMethod("log.sessions", &SessionsRpc, nullptr, kRpcServerThread);
    core::AddRoute("/logs/", &RouteLogs, nullptr);
}

}  // namespace melange::oasis::providers
