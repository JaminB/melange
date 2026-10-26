#pragma once
// The pure half of wum.media.resolve in the helper: yt-dlp's --flat-playlist -J output to a `resolved` message, the
// error class of a failed run, and fitting the message into one pipe line. The runner is resolve.cpp.
#include <string>
#include <string_view>

#include "json_lite.h"
#include "media/pipe_codec.h"
#include "media/url_logic.h"

namespace holo::resolve {
namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;
namespace url = melange::media::url;

inline double CleanDuration(double d) { return d > 0 && d < 1e7 ? d : 0; }

// One entry's video id: its own id, or the id in its url (yt-dlp's flat entries carry both).
inline std::string EntryId(const json::Value& e) {
    std::string id = e.Str("id");
    if (url::ValidVideoId(id)) return id;
    url::Classified c;
    if (url::Classify(e.Str("url"), &c) && c.kind == url::Kind::YouTube && url::ValidVideoId(c.yt.id)) return c.yt.id;
    return {};
}

// False when the output is not yt-dlp JSON at all.
inline bool ParseOutput(std::string_view out, pipe::ResolvedEvt* r) {
    json::Value v;
    if (!json::Parse(out, &v) || v.type != json::Type::Object) return false;
    r->title = url::CleanName(v.Str("title"), proto::kMaxTitleBytes);
    const json::Value* entries = v.Get("entries");
    auto add = [&](const json::Value& e) {
        if (r->items.size() >= proto::kMaxResolveItems || e.type != json::Type::Object) return;
        pipe::ResolveItem it;
        it.id = EntryId(e);
        if (it.id.empty()) return;  // a deleted or private entry has no usable id
        it.url = url::CanonicalVideoUrl(it.id);
        it.title = url::CleanName(e.Str("title"), proto::kMaxTitleBytes);
        it.duration = CleanDuration(e.Num("duration"));
        r->items.push_back(std::move(it));
    };
    if (entries && entries->type == json::Type::Array) {
        for (const json::Value& e : entries->items) add(e);
    } else {
        add(v);
    }
    return true;
}

// The resolve error code for a failed run, from yt-dlp's stderr.
inline std::string ErrorCode(std::string_view err) {
    auto has = [&](std::string_view s) { return err.find(s) != std::string_view::npos; };
    if (has("Private video") || has("Sign in to confirm your age") || has("age-restricted") || has("Video unavailable") ||
        has("This video is unavailable") || has("members-only") || has("has been removed") || has("account associated with this video") ||
        has("This playlist does not exist") || has("The playlist does not exist"))
        return proto::err::kPrivate;
    if (has("getaddrinfo failed") || has("Unable to download webpage") || has("Failed to resolve") || has("timed out") ||
        has("Connection refused") || has("Network is unreachable") || has("urlopen error") || has("Connection reset") ||
        has("Temporary failure in name resolution") || has("No route to host"))
        return proto::err::kNetwork;
    return proto::err::kYtdlp;
}

// The last "ERROR:" line (or the last line), for error.message.
inline std::string ErrorMessage(std::string_view err) {
    std::string_view best;
    size_t i = 0;
    while (i < err.size()) {
        size_t j = err.find('\n', i);
        if (j == std::string_view::npos) j = err.size();
        std::string_view line = err.substr(i, j - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (!line.empty() && (line.starts_with("ERROR:") || best.empty() || !best.starts_with("ERROR:"))) best = line;
        i = j + 1;
    }
    return url::CleanName(best, proto::kMaxMessageBytes);
}

// Shrinks titles, then drops items from the end, until the message fits one pipe line.
inline void FitLine(pipe::ResolvedEvt* r) {
    size_t cap = proto::kMaxTitleBytes;
    for (;;) {
        if (pipe::Encode(*r).size() - 1 <= proto::kMaxLine) return;
        if (cap > 16) {
            cap /= 2;
            for (auto& it : r->items) it.title = url::CleanName(it.title, cap);
            r->title = url::CleanName(r->title, cap);
        } else if (!r->items.empty()) {
            r->items.pop_back();
        } else {
            r->title.clear();
            return;
        }
    }
}
}  // namespace holo::resolve
