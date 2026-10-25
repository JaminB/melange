#pragma once
// The hardened mpv and yt-dlp options (design 1.1.4, Critique S1/S3/Y3), in one place and pure so the logic test pins
// them. Nothing a mod or a peer sends becomes an option here: the only inputs are the core's paths, the core's ini
// values (range-checked by pipe_codec.h), a height and a language code that passes url::ValidLang again.
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "media/media_proto.h"
#include "media/url_logic.h"

namespace holo::opts {
namespace url = melange::media::url;

struct MpvOption {
    const char* name;
    std::string value;
    bool optional;  // a built-in script toggle an older (AllowCustomLibmpv) libmpv may not have; missing = no script
};
using Pairs = std::vector<std::pair<std::string, std::string>>;

// [Media] Hwdec values the helper accepts (the copy-back modes the software renderer can use, plus auto and no).
inline std::string SafeHwdec(const std::string& h) {
    for (const char* ok : {"auto-copy", "auto", "no", "d3d11va-copy", "dxva2-copy", "nvdec-copy"})
        if (h == ok) return h;
    return "auto-copy";
}

// Global options, set before mpv_initialize. configDir is <cache>\mpvhome (empty), UTF-8.
// caFile: the PEM bundle of the Windows trusted roots the helper writes into its cache (sandbox::WriteRootBundle). At
// Low integrity libcurl cannot import the Windows store itself (it opens it writable), so without the file every https
// stream fails verification; verification itself is never turned off.
inline std::vector<MpvOption> BaseOptions(const std::string& configDir, const std::string& hwdec, int audioDelayMs,
                                          const std::string& caFile = {}) {
    char delay[32];
    snprintf(delay, sizeof delay, "%+.3f", (audioDelayMs < -500 ? -500 : audioDelayMs > 500 ? 500 : audioDelayMs) / 1000.0);
    std::vector<MpvOption> o = {
        // no user configuration, scripts or files of any kind
        {"config", "no", false},
        {"config-dir", configDir, false},
        {"load-scripts", "no", false},
        {"osc", "no", false},
        {"load-stats-overlay", "no", true},
        {"load-console", "no", true},
        {"load-select", "no", true},
        {"load-commands", "no", true},
        {"load-auto-profiles", "no", true},
        {"load-positioning", "no", true},
        {"load-context-menu", "no", true},
        {"input-default-bindings", "no", false},
        {"input-vo-keyboard", "no", true},
        {"input-ipc-server", "", false},
        {"input-media-keys", "no", true},
        {"terminal", "no", false},
        {"idle", "yes", false},
        {"keep-open", "yes", false},
        {"save-position-on-quit", "no", false},
        {"resume-playback", "no", false},
        {"use-filedir-conf", "no", false},
        {"access-references", "no", false},
        {"ordered-chapters", "no", false},
        {"load-unsafe-playlists", "no", false},
        {"sub-auto", "no", false},
        {"audio-file-auto", "no", false},
        {"cover-art-auto", "no", false},
        {"stop-screensaver", "no", true},
        {"osd-level", "0", false},  // no OSD burned into the frames (subtitles are not OSD)
        {"osd-bar", "no", true},
        // output
        {"vo", "libmpv", false},
        {"hwdec", SafeHwdec(hwdec), false},
        {"cache", "yes", false},
        {"demuxer-max-bytes", "64MiB", false},
        {"demuxer-max-back-bytes", "32MiB", false},
        {"audio-client-name", "Holo (Worms Ultimate Mayhem)", false},
        {"audio-delay", delay, false},
        // the built-in ytdl_hook is loaded; each item sets ytdl itself (no for torrent items)
        {"ytdl", "yes", false},
        {"tls-verify", "yes", false},
    };
    if (!caFile.empty()) o.push_back({"tls-ca-file", caFile, false});
    return o;
}

// Per item: "yes" only for a YouTube item (ytdl_hook's own edl:// of the video, audio and subtitle streams); every
// other item, and the global default above, is "no".
inline const char* AccessReferences(bool youtubeItem) { return youtubeItem ? "yes" : "no"; }

// script-opts for the built-in ytdl_hook, as separate pairs (a comma in a path cannot split them).
inline Pairs ScriptOpts(const std::string& ytdlpPath) {
    return {{"ytdl_hook-ytdl_path", ytdlpPath}, {"ytdl_hook-try_ytdl_first", "yes"}};
}

// yt-dlp flags from the fixed whitelist (S1): no config files, plugins, exec hooks or cache; YouTube extractors only;
// deno by full path. A subtitle language is added only when it is a plain language code.
inline Pairs YtdlRawOptions(const std::string& denoPath, const std::string& slang) {
    Pairs p = {{"ignore-config", ""},
               {"no-plugin-dirs", ""},
               {"no-exec", ""},
               {"no-cache-dir", ""},
               {"no-mark-watched", ""},
               {"use-extractors", "youtube,youtube:tab"},
               {"js-runtimes", "deno:" + denoPath}};
    if (!slang.empty() && url::ValidLang(slang)) {
        p.push_back({"sub-langs", slang + ".*"});
        p.push_back({"write-subs", ""});
        p.push_back({"write-auto-subs", ""});
    }
    return p;
}

// The format: every alternative keeps the height cap (Y3); SDR only; no AV1 (the software path decodes it slowly).
inline int CapHeight(int requested, int maxHeight) {
    int h = requested < maxHeight ? requested : maxHeight;
    return h < 144 ? 144 : h > 2160 ? 2160 : h;
}
inline std::string YtdlFormat(int height) {
    const std::string h = std::to_string(height);
    return "bv*[height<=" + h + "][vcodec!^=av01][dynamic_range=SDR]+ba/b[height<=" + h + "]/bv*[height<=" + h +
           "]+ba/wv*+ba/w";
}

// yt-dlp for wum.media.resolve: the same whitelist as flags, a list cap, then "--" before the canonical URL.
inline std::vector<std::wstring> ResolveArgv(const std::wstring& ytdlp, const std::wstring& deno, const std::wstring& canonicalUrl) {
    std::vector<std::wstring> a = {ytdlp};
    for (const auto& [k, v] : YtdlRawOptions("", "")) {
        if (k == "js-runtimes") continue;  // added below with the wide path
        a.push_back(L"--" + std::wstring(k.begin(), k.end()));
        if (!v.empty()) a.push_back(std::wstring(v.begin(), v.end()));
    }
    a.push_back(L"--js-runtimes");
    a.push_back(L"deno:" + deno);
    a.insert(a.end(), {L"--flat-playlist", L"-J", L"--no-warnings", L"--playlist-end",
                       std::to_wstring(melange::media::proto::kMaxResolveItems), L"--", canonicalUrl});
    return a;
}
}  // namespace holo::opts
