#pragma once
// The helper's command line (docs/media-helper.md "Starting the helper"). Exactly the proto::arg flags, each at most
// once; anything else, a missing value, a relative path or a pipe outside melange-holo-* refuses to start. Pure, so
// the logic test covers it. --play <file> is a developer check for --selftest only (never passed by the core): it
// also plays one local video file through the real player path into a frame section.
#include <cstdint>
#include <string>
#include <string_view>

#include "media/media_proto.h"

namespace holo {
struct Args {
    std::wstring pipe, cache, ytdlp, deno, libmpv, play;
    uint32_t parent = 0;
    bool sweep = false, selftest = false;
};

inline bool AbsolutePath(std::wstring_view p) {
    if (p.size() < 3 || p.size() > 1024) return false;
    const bool drive = ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && p[2] == L'\\';
    if (!drive) return false;
    for (wchar_t c : p)
        if (c < 0x20 || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'?' || c == L'*') return false;
    for (size_t i = 0; (i = p.find(L"\\..", i)) != std::wstring_view::npos; ++i)
        if (i + 3 == p.size() || p[i + 3] == L'\\') return false;  // no parent segments
    return p.find(L':', 2) == std::wstring_view::npos;           // no alternate data streams
}
inline bool PipeName(std::wstring_view p) {
    constexpr std::wstring_view pre = L"\\\\.\\pipe\\melange-holo-";
    if (p.size() <= pre.size() || p.size() > 128 || p.substr(0, pre.size()) != pre) return false;
    for (wchar_t c : p.substr(pre.size()))
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-')) return false;
    return true;
}

// argv[1..argc). False with a reason for anything the core would never pass.
inline bool ParseArgs(int argc, const wchar_t* const* argv, Args* out, std::string* why) {
    namespace a = melange::media::proto::arg;
    Args r;
    bool seen[8] = {};
    constexpr const wchar_t kPlay[] = L"--play";
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view k = argv[i];
        if (k == a::kSweep || k == a::kSelftest) {
            bool& b = k == a::kSweep ? r.sweep : r.selftest;
            if (b) return fail("flag given twice");
            b = true;
            continue;
        }
        struct Slot { const wchar_t* name; std::wstring* dst; };
        const Slot slots[] = {{a::kPipe, &r.pipe}, {a::kCache, &r.cache}, {a::kYtdlp, &r.ytdlp}, {a::kDeno, &r.deno},
                              {a::kLibmpv, &r.libmpv}, {a::kParent, nullptr}, {kPlay, &r.play}};
        int which = -1;
        for (int s = 0; s < 7; ++s)
            if (k == slots[s].name) which = s;
        if (which < 0) return fail("unknown argument");
        if (seen[which]) return fail("flag given twice");
        seen[which] = true;
        if (i + 1 >= argc || argv[i + 1][0] == L'\0') return fail("flag without a value");
        const std::wstring_view v = argv[++i];
        if (which == 5) {
            uint64_t pid = 0;
            if (v.size() > 10) return fail("bad --parent");
            for (wchar_t c : v) {
                if (c < L'0' || c > L'9') return fail("bad --parent");
                pid = pid * 10 + static_cast<uint64_t>(c - L'0');
            }
            if (pid == 0 || pid > 0xFFFFFFFFull) return fail("bad --parent");
            r.parent = static_cast<uint32_t>(pid);
        } else if (which == 0) {
            if (!PipeName(v)) return fail("bad --pipe");
            r.pipe = v;
        } else {
            if (!AbsolutePath(v)) return fail("paths must be absolute");
            *slots[which].dst = v;
        }
    }
    if (r.selftest) {
        if (!r.pipe.empty() || r.parent || r.sweep) return fail("--selftest runs without a pipe");
    } else if (!r.play.empty()) {
        return fail("--play is for --selftest only");
    } else if (r.pipe.empty() || !r.parent || r.cache.empty() || r.libmpv.empty() || r.ytdlp.empty() || r.deno.empty()) {
        return fail("missing a required flag (--pipe --parent --cache --libmpv --ytdlp --deno)");
    }
    *out = std::move(r);
    return true;
}
}  // namespace holo
