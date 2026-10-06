#pragma once
// The game folder's local.cfg (and Default.cfg): the engine switches the stock launcher writes, usually one line
// like "/W:1280 /H:720 /REFRESH:59 /SSAA:1 /SHADOWMAP:1024 /CONFIG:user.cfg". The engine reads Default.cfg and then
// local.cfg after the command line, so these are the window size the game opens at. Read and rewritten as text:
// unknown switches, their order, spacing and line endings are kept. Pure (no file access), self-tested in
// tests/display_selftest.cpp. Also the window-size list Settings › Display offers.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace melange::launcher::localcfg {
struct Token {
    size_t start = 0, end = 0;  // [start, end) in the text
    std::string key;            // "/W" (upper case, up to ':'); empty for a word that is not a switch
    std::string value;          // after ':' ("1280"), or the next word for "/W 1280" (see Info)
};

inline bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool Digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

inline std::vector<Token> Tokenize(std::string_view text) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && Space(text[i])) ++i;
        if (i >= text.size()) break;
        Token t;
        t.start = i;
        while (i < text.size() && !Space(text[i])) ++i;
        t.end = i;
        const std::string_view w = text.substr(t.start, t.end - t.start);
        if (w[0] == '/') {
            const size_t colon = w.find(':');
            std::string k(w.substr(0, colon));
            for (char& c : k) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            t.key = k;
            if (colon != std::string_view::npos) t.value = std::string(w.substr(colon + 1));
        }
        out.push_back(std::move(t));
    }
    return out;
}

struct Info {
    int w = 0, h = 0;      // 0 = not set here
    bool fs = false;       // /FS: the engine's own exclusive fullscreen (the stock launcher's "Fullscreen")
    bool win = false;      // /WIN
};

// "/W:1280" or "/W 1280"; the last one wins, as the engine applies them in order.
inline Info Read(std::string_view text) {
    Info r;
    const std::vector<Token> t = Tokenize(text);
    for (size_t i = 0; i < t.size(); ++i) {
        std::string v = t[i].value;
        if (v.empty() && i + 1 < t.size() && t[i + 1].key.empty()) v = std::string(text.substr(t[i + 1].start, t[i + 1].end - t[i + 1].start));
        if (t[i].key == "/W" && Digits(v)) r.w = atoi(v.c_str());
        else if (t[i].key == "/H" && Digits(v)) r.h = atoi(v.c_str());
        else if (t[i].key == "/FS") r.fs = true, r.win = false;
        else if (t[i].key == "/WIN") r.win = true, r.fs = false;
    }
    return r;
}

// `text` with the window size set to w x h, every /FS removed when `removeFs` (Melange's borderless fullscreen
// replaces it; the two conflict), and everything else as it was. A missing /W or /H is added after the last switch,
// in the file's own style ("/W:1280" unless the file writes "/W 1280"). *removedFs: whether an /FS went.
inline std::string Rewrite(std::string_view text, int w, int h, bool removeFs, bool* removedFs = nullptr) {
    if (removedFs) *removedFs = false;
    const std::vector<Token> t = Tokenize(text);
    struct Edit {
        size_t start, end;
        std::string with;
    };
    std::vector<Edit> edits;
    bool haveW = false, haveH = false, spaced = false;
    for (size_t i = 0; i < t.size(); ++i) {
        const bool isW = t[i].key == "/W", isH = t[i].key == "/H";
        if (isW || isH) {
            const std::string n = std::to_string(isW ? w : h);
            (isW ? haveW : haveH) = true;
            if (!t[i].value.empty() || text[t[i].end - 1] == ':') {
                const size_t colon = text.find(':', t[i].start);
                edits.push_back({colon + 1, t[i].end, n});
            } else if (i + 1 < t.size() && t[i + 1].key.empty() && Digits(text.substr(t[i + 1].start, t[i + 1].end - t[i + 1].start))) {
                spaced = true;
                edits.push_back({t[i + 1].start, t[i + 1].end, n});
                ++i;
            } else {
                edits.push_back({t[i].end, t[i].end, ":" + n});
            }
        } else if (removeFs && t[i].key == "/FS") {
            // The switch and the blanks before it (or after it, when it is the first word).
            size_t a = t[i].start, b = t[i].end;
            while (a > 0 && (text[a - 1] == ' ' || text[a - 1] == '\t')) --a;
            if (a == 0 || text[a - 1] == '\n' || text[a - 1] == '\r') {
                a = t[i].start;
                while (b < text.size() && (text[b] == ' ' || text[b] == '\t')) ++b;
            }
            edits.push_back({a, b, ""});
            if (removedFs) *removedFs = true;
        }
    }
    std::string add;
    const char* sep = spaced ? " " : ":";
    if (!haveW) add += std::string(add.empty() ? "" : " ") + "/W" + sep + std::to_string(w);
    if (!haveH) add += std::string(add.empty() ? "" : " ") + "/H" + sep + std::to_string(h);
    std::string out;
    size_t at = 0;
    for (const Edit& e : edits) {
        out.append(text.substr(at, e.start - at));
        out += e.with;
        at = e.end;
    }
    // Additions go right after the last word, before any trailing newline.
    const size_t tail = t.empty() ? 0 : t.back().end;
    if (tail > at) {
        out.append(text.substr(at, tail - at));
        at = tail;
    }
    if (!add.empty()) out += (out.empty() || Space(out.back()) ? "" : " ") + add;
    out.append(text.substr(at));
    return out;
}

struct Size {
    int w = 0, h = 0;
};
inline bool operator==(const Size& a, const Size& b) { return a.w == b.w && a.h == b.h; }

// A window size the engine can be given: at least 640x480 and no wider than a GL texture can ever be.
inline bool ValidSize(int w, int h) { return w >= 640 && h >= 480 && w <= 16384 && h <= 16384; }

// The window sizes to offer: the monitor's reported display modes (EnumDisplaySettings) and the common 16:9 sizes,
// each no larger than the monitor and at least 640x480, without duplicates, largest first.
inline std::vector<Size> Modes(const std::vector<Size>& reported, int monW, int monH) {
    static const Size kCommon[] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
    std::vector<Size> out;
    auto add = [&](Size s) {
        if (!ValidSize(s.w, s.h) || (monW > 0 && s.w > monW) || (monH > 0 && s.h > monH)) return;
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    for (const Size& s : reported) add(s);
    for (const Size& s : kCommon) add(s);
    std::sort(out.begin(), out.end(), [](const Size& a, const Size& b) { return a.w != b.w ? a.w > b.w : a.h > b.h; });
    return out;
}
}  // namespace melange::launcher::localcfg
