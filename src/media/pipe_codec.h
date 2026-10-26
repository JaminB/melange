#pragma once
// The pipe's wire format: one JSON object per line, "t" names the message (media_proto.h). Typed messages, a bounded
// JSON reader and writer, and the line splitter. Pure and header-only; compiled into the x86 core and the x64 helper.
// Frozen for Melange 0.12.0 (docs/media-helper.md).
//
// Every line is untrusted input on both sides (the helper parses hostile media; Critique S11): the reader caps the
// line, the depth and the node count, Decode range-checks every number, caps every string, refuses names it does not
// know, and rebuilds what it can from ids (a resolved item's url is rebuilt from its video id). Decode never throws
// on bad input; it returns false and a short reason, and the caller ignores the line.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "media/media_proto.h"
#include "media/url_logic.h"

namespace melange::media::pipe {
// ---------------------------------------------------------------- bounded JSON
enum class JType : uint8_t { Null, Bool, Number, String, Array, Object };
struct JValue {
    JType type = JType::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JValue> items;
    std::vector<std::pair<std::string, JValue>> members;
    const JValue* Get(std::string_view key) const {
        if (type != JType::Object) return nullptr;
        for (const auto& [k, v] : members)
            if (k == key) return &v;
        return nullptr;
    }
};

class JReader {
public:
    explicit JReader(std::string_view text) : p_(text.data()), end_(text.data() + text.size()) {}
    bool Parse(JValue* out, std::string* why) {
        if (static_cast<size_t>(end_ - p_) > proto::kMaxLine) return Fail(why, "line too long");
        Ws();
        if (!Value(out, 0)) return Fail(why, why_ ? why_ : "malformed");
        Ws();
        if (p_ != end_) return Fail(why, "trailing data");
        return true;
    }

private:
    bool Fail(std::string* why, const char* w) {
        if (why) *why = w;
        return false;
    }
    bool Err(const char* w) {
        if (!why_) why_ = w;
        return false;
    }
    void Ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }
    bool Lit(const char* w) {
        const size_t n = std::strlen(w);
        if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, w, n) != 0) return Err("bad literal");
        p_ += n;
        return true;
    }
    bool Value(JValue* v, int depth) {
        if (depth > proto::kMaxDepth) return Err("too deep");
        if (++nodes_ > proto::kMaxNodes) return Err("too many values");
        if (p_ >= end_) return Err("unexpected end");
        switch (*p_) {
            case '{': return Object(v, depth);
            case '[': return Array(v, depth);
            case '"': v->type = JType::String; return String(&v->s);
            case 't': v->type = JType::Bool; v->b = true; return Lit("true");
            case 'f': v->type = JType::Bool; v->b = false; return Lit("false");
            case 'n': v->type = JType::Null; return Lit("null");
            default: v->type = JType::Number; return Number(&v->n);
        }
    }
    bool Object(JValue* v, int depth) {
        v->type = JType::Object;
        ++p_;
        Ws();
        if (p_ < end_ && *p_ == '}') return ++p_, true;
        for (;;) {
            Ws();
            std::string key;
            if (p_ >= end_ || *p_ != '"' || !String(&key)) return Err("bad key");
            for (const auto& m : v->members)
                if (m.first == key) return Err("duplicate key");
            Ws();
            if (p_ >= end_ || *p_ != ':') return Err("missing ':'");
            ++p_;
            Ws();
            v->members.emplace_back(std::move(key), JValue{});
            if (!Value(&v->members.back().second, depth + 1)) return false;
            Ws();
            if (p_ < end_ && *p_ == ',') { ++p_; continue; }
            if (p_ < end_ && *p_ == '}') return ++p_, true;
            return Err("bad object");
        }
    }
    bool Array(JValue* v, int depth) {
        v->type = JType::Array;
        ++p_;
        Ws();
        if (p_ < end_ && *p_ == ']') return ++p_, true;
        for (;;) {
            Ws();
            v->items.emplace_back();
            if (!Value(&v->items.back(), depth + 1)) return false;
            Ws();
            if (p_ < end_ && *p_ == ',') { ++p_; continue; }
            if (p_ < end_ && *p_ == ']') return ++p_, true;
            return Err("bad array");
        }
    }
    static void Utf8(uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool Hex4(uint32_t* cp) {
        if (end_ - p_ < 4) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        *cp = v;
        return true;
    }
    bool String(std::string* out) {
        ++p_;  // the opening quote
        while (p_ < end_) {
            const char c = *p_++;
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Err("control character in string");
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (p_ >= end_) break;
            const char e = *p_++;
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(&cp)) return Err("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo;
                        if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return Err("lone surrogate");
                        p_ += 2;
                        if (!Hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return Err("lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Err("lone surrogate");
                    }
                    Utf8(cp, out);
                    break;
                }
                default: return Err("bad escape");
            }
        }
        return Err("unterminated string");
    }
    bool Number(double* out) {
        const char* s = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        if (p_ >= end_ || *p_ < '0' || *p_ > '9') return Err("bad number");
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' || *p_ == '-')) ++p_;
        const auto r = std::from_chars(s, p_, *out);
        if (r.ec != std::errc() || r.ptr != p_ || !std::isfinite(*out)) return Err("bad number");
        return true;
    }

    const char* p_;
    const char* end_;
    size_t nodes_ = 0;
    const char* why_ = nullptr;
};

inline bool ParseJson(std::string_view text, JValue* out, std::string* why) { return JReader(text).Parse(out, why); }

// JSON text for one string, with every control character, quote and backslash escaped.
inline void AppendString(std::string* out, std::string_view s) {
    static const char kHex[] = "0123456789abcdef";
    out->push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\n': *out += "\\n"; break;
            case '\r': *out += "\\r"; break;
            case '\t': *out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    *out += "\\u00";
                    out->push_back(kHex[c >> 4]);
                    out->push_back(kHex[c & 15]);
                } else {
                    out->push_back(static_cast<char>(c));
                }
        }
    }
    out->push_back('"');
}
inline void AppendNumber(std::string* out, double v) {
    if (!std::isfinite(v)) v = 0;  // JSON has no NaN or infinity
    char b[32];
    const auto r = std::to_chars(b, b + sizeof b, v);
    out->append(b, r.ptr);
}

// Builds one object. Keys are written as given (they are literals in this file).
class Obj {
public:
    Obj() : s_("{") {}
    Obj& Str(const char* k, std::string_view v) { Key(k); AppendString(&s_, v); return *this; }
    Obj& Num(const char* k, double v) { Key(k); AppendNumber(&s_, v); return *this; }
    Obj& Bool(const char* k, bool v) { Key(k); s_ += v ? "true" : "false"; return *this; }
    Obj& Null(const char* k) { Key(k); s_ += "null"; return *this; }
    Obj& Raw(const char* k, std::string_view json) { Key(k); s_ += json; return *this; }
    std::string End() { s_ += '}'; return std::move(s_); }

private:
    void Key(const char* k) {
        if (s_.size() > 1) s_ += ',';
        AppendString(&s_, k);
        s_ += ':';
    }
    std::string s_;
};

// ---------------------------------------------------------------- messages
enum class Dir : uint8_t { ToHelper, ToCore };

// core -> helper
struct HelloCmd {
    int proto = proto::kProto;
    std::string hwdec = "auto-copy";  // [Media] Hwdec: auto-copy | auto | no | d3d11va-copy | dxva2-copy | nvdec-copy
    int audioDelayMs = 25;            // [Media] AudioDelayMs, -500..500
    int maxHeight = 1080;             // [Media] MaxHeight
};
struct OpenCmd {
    uint32_t id = 0, gen = 0;  // player id (core-assigned) and load generation (each open/reopen of that player)
    std::string shm;           // the player's frame section name, created by the core (Local\melange-holo-...)
    std::string url;           // canonical YouTube video URL, or empty for a torrent item
    std::string ih;            // torrent infohash (bt.add'ed before), or empty
    int file = -1;             // torrent file index
    double start = 0;
    bool paused = true, mute = false;
    int volume = 70;
    double speed = 1.0;
    int height = 720;           // format cap
    std::string alang, slang;  // ValidLang or empty
};
struct SetCmd {
    uint32_t id = 0;
    std::string prop;     // setprop::*
    bool isBool = false;  // pause, mute: boolean; aid/sid: false = off
    bool boolean = false;
    double number = 0;    // volume, speed, aid/sid track id
};
struct SeekCmd {
    uint32_t id = 0;
    double t = 0;                // seconds (relative: delta); "pos" on the wire, since "t" names the message
    std::string mode = "exact";  // exact | keyframes | relative
};
struct SizeCmd {
    uint32_t id = 0;
    int w = 0, h = 0;  // render target hint, at most kFrameMaxW x kFrameMaxH
};
struct CloseCmd { uint32_t id = 0; };
struct ResolveCmd {
    uint32_t rid = 0;
    std::string url;  // canonical video or playlist URL
};
struct BtAddCmd {
    std::string ih, magnet;  // magnet: canonical (url::CanonicalMagnet)
    int uploadKBps = 1024;
    bool upnp = false;
    int port = 0;  // 0 = random
};
struct BtSelectCmd {
    std::string ih;
    int index = 0;
};
struct BtRemoveCmd { std::string ih; };
struct QuitCmd {};
struct PingCmd {};

// helper -> core
struct HelloEvt {
    int proto = proto::kProto;
    std::string mpv, ffmpeg, libtorrent, hwdec;  // versions, for the Melange/Media panel and the log
};
struct Track {
    int id = 0;
    std::string type;  // audio | sub | video
    std::string lang, title;
    bool selected = false;
};
struct VideoParams {
    int w = 0, h = 0;
    double aspect = 0;
};
struct PropEvt {
    enum class Kind : uint8_t { None, Number, Bool, String, Tracks, Video };
    uint32_t id = 0, gen = 0;
    std::string name;  // prop::*
    Kind kind = Kind::None;  // None: mpv reports the property as unavailable
    double number = 0;
    bool boolean = false;
    std::string str;
    double qpcMs = 0;  // time-pos / audio-pts: the helper's QueryPerformanceCounter in ms when the value was read
    std::vector<Track> tracks;
    VideoParams video;
};
struct EventEvt {
    uint32_t id = 0, gen = 0;
    std::string name;    // event::*
    std::string reason;  // end-file: eof | error | stop
};
struct ErrorEvt {
    uint32_t id = 0, gen = 0, rid = 0;  // whichever applies; 0 = none
    std::string ih;
    std::string code, message;
};
struct ResolveItem {
    std::string url, id, title;
    double duration = 0;  // seconds, 0 = unknown
};
struct ResolvedEvt {
    uint32_t rid = 0;
    std::string title;
    std::vector<ResolveItem> items;
    std::string error, message;  // error: err::kYtdlp | kNetwork | kPrivate | kTimeout; items empty then
};
struct BtFile {
    int index = 0;
    std::string path;
    double size = 0;  // bytes
    bool video = false;
};
struct BtFilesEvt {
    std::string ih, name;
    std::vector<BtFile> files;
};
struct BtStatusEvt {
    std::string ih;
    double down = 0, up = 0;  // bytes/s
    int peers = 0;
    double progress = 0, have = 0;  // selected file 0..1, bytes
    bool metadata = false;
};
struct BtRemovedEvt { std::string ih; };
struct PongEvt {};

using Message = std::variant<HelloCmd, OpenCmd, SetCmd, SeekCmd, SizeCmd, CloseCmd, ResolveCmd, BtAddCmd, BtSelectCmd,
                             BtRemoveCmd, QuitCmd, PingCmd, HelloEvt, PropEvt, EventEvt, ErrorEvt, ResolvedEvt, BtFilesEvt,
                             BtStatusEvt, BtRemovedEvt, PongEvt>;

// ---------------------------------------------------------------- encode
namespace detail {
inline std::string Line(Obj o) { return o.End() + "\n"; }
inline std::string TrackJson(const Track& t) {
    return Obj().Num("id", t.id).Str("type", t.type).Str("lang", t.lang).Str("title", t.title).Bool("selected", t.selected).End();
}
template <class T, class F>
std::string ArrayJson(const std::vector<T>& v, F&& one) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += one(v[i]);
    }
    return s + "]";
}

inline std::string Enc(const HelloCmd& m) {
    return Line(Obj().Str("t", proto::msg::kHello).Num("proto", m.proto).Str("hwdec", m.hwdec).Num("audioDelayMs", m.audioDelayMs)
                    .Num("maxHeight", m.maxHeight));
}
inline std::string Enc(const OpenCmd& m) {
    Obj o;
    o.Str("t", proto::msg::kOpen).Num("id", m.id).Num("gen", m.gen).Str("shm", m.shm);
    if (!m.url.empty()) o.Str("url", m.url);
    else o.Raw("bt", Obj().Str("ih", m.ih).Num("index", m.file).End());
    o.Num("start", m.start).Bool("paused", m.paused).Num("volume", m.volume).Bool("mute", m.mute).Num("speed", m.speed)
        .Num("height", m.height).Str("alang", m.alang).Str("slang", m.slang);
    return Line(std::move(o));
}
inline std::string Enc(const SetCmd& m) {
    Obj o;
    o.Str("t", proto::msg::kSet).Num("id", m.id).Str("prop", m.prop);
    if (m.isBool) o.Bool("value", m.boolean);
    else o.Num("value", m.number);
    return Line(std::move(o));
}
inline std::string Enc(const SeekCmd& m) { return Line(Obj().Str("t", proto::msg::kSeek).Num("id", m.id).Num("pos", m.t).Str("mode", m.mode)); }
inline std::string Enc(const SizeCmd& m) { return Line(Obj().Str("t", proto::msg::kSize).Num("id", m.id).Num("w", m.w).Num("h", m.h)); }
inline std::string Enc(const CloseCmd& m) { return Line(Obj().Str("t", proto::msg::kClose).Num("id", m.id)); }
inline std::string Enc(const ResolveCmd& m) { return Line(Obj().Str("t", proto::msg::kResolve).Num("rid", m.rid).Str("url", m.url)); }
inline std::string Enc(const BtAddCmd& m) {
    return Line(Obj().Str("t", proto::msg::kBtAdd).Str("ih", m.ih).Str("magnet", m.magnet).Num("uploadKBps", m.uploadKBps)
                    .Bool("upnp", m.upnp).Num("port", m.port));
}
inline std::string Enc(const BtSelectCmd& m) { return Line(Obj().Str("t", proto::msg::kBtSelect).Str("ih", m.ih).Num("index", m.index)); }
inline std::string Enc(const BtRemoveCmd& m) { return Line(Obj().Str("t", proto::msg::kBtRemove).Str("ih", m.ih)); }
inline std::string Enc(const QuitCmd&) { return Line(Obj().Str("t", proto::msg::kQuit)); }
inline std::string Enc(const PingCmd&) { return Line(Obj().Str("t", proto::msg::kPing)); }
inline std::string Enc(const HelloEvt& m) {
    return Line(Obj().Str("t", proto::msg::kHello).Num("proto", m.proto).Str("mpv", m.mpv).Str("ffmpeg", m.ffmpeg)
                    .Str("libtorrent", m.libtorrent).Str("hwdec", m.hwdec));
}
inline std::string Enc(const PropEvt& m) {
    Obj o;
    o.Str("t", proto::msg::kProp).Num("id", m.id).Num("gen", m.gen).Str("name", m.name);
    switch (m.kind) {
        case PropEvt::Kind::None: o.Null("value"); break;
        case PropEvt::Kind::Number: o.Num("value", m.number); break;
        case PropEvt::Kind::Bool: o.Bool("value", m.boolean); break;
        case PropEvt::Kind::String: o.Str("value", m.str); break;
        case PropEvt::Kind::Tracks: o.Raw("value", ArrayJson(m.tracks, TrackJson)); break;
        case PropEvt::Kind::Video:
            o.Raw("value", Obj().Num("w", m.video.w).Num("h", m.video.h).Num("aspect", m.video.aspect).End());
            break;
    }
    if (m.qpcMs > 0) o.Num("qpc", m.qpcMs);
    return Line(std::move(o));
}
inline std::string Enc(const EventEvt& m) {
    return Line(Obj().Str("t", proto::msg::kEvent).Num("id", m.id).Num("gen", m.gen).Str("name", m.name).Str("reason", m.reason));
}
inline std::string Enc(const ErrorEvt& m) {
    return Line(Obj().Str("t", proto::msg::kError).Num("id", m.id).Num("gen", m.gen).Num("rid", m.rid).Str("ih", m.ih)
                    .Str("code", m.code).Str("message", m.message));
}
inline std::string Enc(const ResolvedEvt& m) {
    Obj o;
    o.Str("t", proto::msg::kResolved).Num("rid", m.rid);
    if (!m.error.empty()) {
        o.Str("error", m.error).Str("message", m.message);
    } else {
        o.Str("title", m.title).Raw("items", ArrayJson(m.items, [](const ResolveItem& i) {
            return Obj().Str("url", i.url).Str("id", i.id).Str("title", i.title).Num("duration", i.duration).End();
        }));
    }
    return Line(std::move(o));
}
inline std::string Enc(const BtFilesEvt& m) {
    return Line(Obj().Str("t", proto::msg::kBtFiles).Str("ih", m.ih).Str("name", m.name).Raw("files", ArrayJson(m.files, [](const BtFile& f) {
        return Obj().Num("index", f.index).Str("path", f.path).Num("size", f.size).Bool("video", f.video).End();
    })));
}
inline std::string Enc(const BtStatusEvt& m) {
    return Line(Obj().Str("t", proto::msg::kBtStatus).Str("ih", m.ih).Num("down", m.down).Num("up", m.up).Num("peers", m.peers)
                    .Num("progress", m.progress).Num("have", m.have).Bool("metadata", m.metadata));
}
inline std::string Enc(const BtRemovedEvt& m) { return Line(Obj().Str("t", proto::msg::kBtRemoved).Str("ih", m.ih)); }
inline std::string Enc(const PongEvt&) { return Line(Obj().Str("t", proto::msg::kPong)); }
}  // namespace detail

// One line, '\n' included.
inline std::string Encode(const Message& m) {
    return std::visit([](const auto& x) { return detail::Enc(x); }, m);
}

// ---------------------------------------------------------------- decode
namespace detail {
struct In {
    const JValue& o;
    const char* why = nullptr;
    bool Fail(const char* w) {
        if (!why) why = w;
        return false;
    }
    const JValue* Need(const char* k, JType t) {
        const JValue* v = o.Get(k);
        if (!v || v->type != t) {
            Fail(k);
            return nullptr;
        }
        return v;
    }
    // A number in [lo, hi]; out-of-range values are refused, not clamped (a helper sending them is broken).
    bool NumIn(const char* k, double lo, double hi, double* out, bool optional = false) {
        const JValue* v = o.Get(k);
        if (!v || v->type == JType::Null) return optional || Fail(k);
        if (v->type != JType::Number || !(v->n >= lo && v->n <= hi)) return Fail(k);
        *out = v->n;
        return true;
    }
    template <class I>
    bool IntIn(const char* k, double lo, double hi, I* out, bool optional = false) {
        double d = 0;
        const bool had = o.Get(k) != nullptr;
        if (!NumIn(k, lo, hi, &d, optional)) return false;
        if (!had) return true;
        if (d != std::floor(d)) return Fail(k);
        *out = static_cast<I>(d);
        return true;
    }
    bool Str(const char* k, size_t cap, std::string* out, bool optional = false) {
        const JValue* v = o.Get(k);
        if (!v) return optional || Fail(k);
        if (v->type != JType::String || v->s.size() > cap) return Fail(k);
        *out = v->s;
        return true;
    }
    bool Bool(const char* k, bool* out, bool optional = false) {
        const JValue* v = o.Get(k);
        if (!v) return optional || Fail(k);
        if (v->type != JType::Bool) return Fail(k);
        *out = v->b;
        return true;
    }
    bool Ih(const char* k, std::string* out, bool optional = false) {
        const JValue* v = o.Get(k);
        if (!v || (optional && v->type == JType::String && v->s.empty())) return optional || Fail(k);
        if (v->type != JType::String || !url::ValidInfohash(v->s)) return Fail(k);
        *out = v->s;
        return true;
    }
};

inline bool OneOf(std::string_view s, std::initializer_list<const char*> set) {
    for (const char* x : set)
        if (s == x) return true;
    return false;
}
inline bool ShmName(std::string_view s) {  // Local\melange-holo-<pid>-<hex>-<id>
    if (s.empty() || s.size() > 128) return false;
    for (char c : s)
        if (!url::IdChar(c) && c != '\\' && c != '.') return false;
    return true;
}
inline std::string CleanText(std::string s, size_t cap) { return url::CleanName(s, cap); }

inline bool DecToHelper(const std::string& t, In& in, Message* out) {
    namespace m = proto::msg;
    if (t == m::kHello) {
        HelloCmd c;
        if (!in.IntIn("proto", 0, 1000, &c.proto) || !in.Str("hwdec", proto::kMaxShortBytes, &c.hwdec, true) ||
            !in.IntIn("audioDelayMs", -500, 500, &c.audioDelayMs, true) || !in.IntIn("maxHeight", 144, 2160, &c.maxHeight, true))
            return false;
        for (char ch : c.hwdec)
            if (!url::IdChar(ch)) return in.Fail("hwdec");
        *out = c;
    } else if (t == m::kOpen) {
        OpenCmd c;
        if (!in.IntIn("id", 1, 4294967295.0, &c.id) || !in.IntIn("gen", 0, 4294967295.0, &c.gen) || !in.Str("shm", 128, &c.shm) ||
            !ShmName(c.shm))
            return in.Fail("open");
        const JValue* u = in.o.Get("url");
        const JValue* bt = in.o.Get("bt");
        if ((u != nullptr) == (bt != nullptr)) return in.Fail("exactly one of url and bt");
        if (u) {
            url::Classified cl;
            if (u->type != JType::String || !url::Classify(u->s, &cl) || cl.kind != url::Kind::YouTube || cl.yt.id.empty())
                return in.Fail("url");
            c.url = cl.url;  // rebuilt from the parsed id, never the text as sent
        } else {
            if (bt->type != JType::Object) return in.Fail("bt");
            In b{*bt};
            if (!b.Ih("ih", &c.ih) || !b.IntIn("index", 0, static_cast<double>(proto::kMaxTorrentFiles - 1), &c.file)) return in.Fail("bt");
        }
        if (!in.NumIn("start", 0, 1e7, &c.start, true) || !in.Bool("paused", &c.paused, true) ||
            !in.IntIn("volume", 0, 100, &c.volume, true) || !in.Bool("mute", &c.mute, true) || !in.NumIn("speed", 0.25, 4, &c.speed, true) ||
            !in.IntIn("height", 144, 2160, &c.height, true) || !in.Str("alang", proto::kMaxShortBytes, &c.alang, true) ||
            !in.Str("slang", proto::kMaxShortBytes, &c.slang, true))
            return false;
        if (!c.alang.empty() && !url::ValidLang(c.alang)) c.alang.clear();  // dropped, never passed on
        if (!c.slang.empty() && !url::ValidLang(c.slang)) c.slang.clear();
        *out = c;
    } else if (t == m::kSet) {
        SetCmd c;
        if (!in.IntIn("id", 1, 4294967295.0, &c.id) || !in.Str("prop", proto::kMaxShortBytes, &c.prop)) return false;
        const JValue* v = in.o.Get("value");
        if (!v) return in.Fail("value");
        namespace sp = proto::setprop;
        if (c.prop == sp::kPause || c.prop == sp::kMute) {
            if (v->type != JType::Bool) return in.Fail("value");
            c.isBool = true;
            c.boolean = v->b;
        } else if (c.prop == sp::kVolume || c.prop == sp::kSpeed) {
            const double lo = c.prop == sp::kVolume ? 0 : 0.25, hi = c.prop == sp::kVolume ? 100 : 4;
            if (v->type != JType::Number || !(v->n >= lo && v->n <= hi)) return in.Fail("value");
            c.number = v->n;
        } else if (c.prop == sp::kAid || c.prop == sp::kSid) {
            if (v->type == JType::Bool && !v->b) {
                c.isBool = true;
            } else if (v->type == JType::Number && v->n >= 1 && v->n <= 9999 && v->n == std::floor(v->n)) {
                c.number = v->n;
            } else {
                return in.Fail("value");
            }
        } else {
            return in.Fail("prop");
        }
        *out = c;
    } else if (t == m::kSeek) {
        SeekCmd c;
        if (!in.IntIn("id", 1, 4294967295.0, &c.id) || !in.NumIn("pos", -1e7, 1e7, &c.t) || !in.Str("mode", proto::kMaxShortBytes, &c.mode) ||
            !OneOf(c.mode, {"exact", "keyframes", "relative"}) || (c.mode != "relative" && c.t < 0))
            return in.Fail("seek");
        *out = c;
    } else if (t == m::kSize) {
        SizeCmd c;
        if (!in.IntIn("id", 1, 4294967295.0, &c.id) || !in.IntIn("w", 16, proto::kFrameMaxW, &c.w) || !in.IntIn("h", 16, proto::kFrameMaxH, &c.h))
            return false;
        *out = c;
    } else if (t == m::kClose) {
        CloseCmd c;
        if (!in.IntIn("id", 1, 4294967295.0, &c.id)) return false;
        *out = c;
    } else if (t == m::kResolve) {
        ResolveCmd c;
        std::string u;
        url::Classified cl;
        if (!in.IntIn("rid", 1, 4294967295.0, &c.rid) || !in.Str("url", proto::kMaxUrlBytes, &u) || !url::Classify(u, &cl) ||
            cl.kind != url::Kind::YouTube)
            return in.Fail("resolve");
        c.url = cl.url;  // a playlist link resolves the list, a video link the one video
        *out = c;
    } else if (t == m::kBtAdd) {
        BtAddCmd c;
        std::string mag;
        url::Classified cl;
        if (!in.Ih("ih", &c.ih) || !in.Str("magnet", proto::kMaxMagnetBytes, &mag) || !url::Classify(mag, &cl) ||
            cl.kind != url::Kind::Magnet || cl.bt.infohash != c.ih || !in.IntIn("uploadKBps", 0, 1e6, &c.uploadKBps, true) ||
            !in.Bool("upnp", &c.upnp, true) || !in.IntIn("port", 0, 65535, &c.port, true))
            return in.Fail("bt.add");
        c.magnet = cl.url;
        *out = c;
    } else if (t == m::kBtSelect) {
        BtSelectCmd c;
        if (!in.Ih("ih", &c.ih) || !in.IntIn("index", 0, static_cast<double>(proto::kMaxTorrentFiles - 1), &c.index)) return false;
        *out = c;
    } else if (t == m::kBtRemove) {
        BtRemoveCmd c;
        if (!in.Ih("ih", &c.ih)) return false;
        *out = c;
    } else if (t == m::kQuit) {
        *out = QuitCmd{};
    } else if (t == m::kPing) {
        *out = PingCmd{};
    } else {
        return in.Fail("unknown message");
    }
    return true;
}

inline bool DecTracks(const JValue& v, std::vector<Track>* out) {
    if (v.type != JType::Array) return false;
    size_t audio = 0, sub = 0, video = 0;
    for (const JValue& e : v.items) {
        if (e.type != JType::Object) return false;
        In in{e};
        Track t;
        if (!in.IntIn("id", 1, 9999, &t.id) || !in.Str("type", proto::kMaxShortBytes, &t.type) || !OneOf(t.type, {"audio", "sub", "video"}) ||
            !in.Str("lang", proto::kMaxShortBytes, &t.lang, true) || !in.Str("title", proto::kMaxTitleBytes * 4, &t.title, true) ||
            !in.Bool("selected", &t.selected, true))
            return false;
        size_t& n = t.type == "audio" ? audio : t.type == "sub" ? sub : video;
        if (++n > proto::kMaxTracks) continue;  // the rest are dropped, the list stays usable
        if (!t.lang.empty() && !url::ValidLang(t.lang)) t.lang.clear();
        t.title = CleanText(std::move(t.title), proto::kMaxTitleBytes);
        out->push_back(std::move(t));
    }
    return true;
}

inline bool DecToCore(const std::string& t, In& in, Message* out) {
    namespace m = proto::msg;
    namespace pr = proto::prop;
    if (t == m::kHello) {
        HelloEvt e;
        if (!in.IntIn("proto", 0, 1000, &e.proto) || !in.Str("mpv", proto::kMaxShortBytes * 2, &e.mpv, true) ||
            !in.Str("ffmpeg", proto::kMaxShortBytes * 2, &e.ffmpeg, true) || !in.Str("libtorrent", proto::kMaxShortBytes * 2, &e.libtorrent, true) ||
            !in.Str("hwdec", proto::kMaxShortBytes, &e.hwdec, true))
            return false;
        e.mpv = CleanText(std::move(e.mpv), proto::kMaxShortBytes);
        e.ffmpeg = CleanText(std::move(e.ffmpeg), proto::kMaxShortBytes);
        e.libtorrent = CleanText(std::move(e.libtorrent), proto::kMaxShortBytes);
        e.hwdec = CleanText(std::move(e.hwdec), proto::kMaxShortBytes);
        *out = e;
    } else if (t == m::kProp) {
        PropEvt e;
        if (!in.IntIn("id", 1, 4294967295.0, &e.id) || !in.IntIn("gen", 0, 4294967295.0, &e.gen) || !in.Str("name", proto::kMaxShortBytes, &e.name) ||
            !in.NumIn("qpc", 0, 1e15, &e.qpcMs, true))
            return false;
        const JValue* v = in.o.Get("value");
        if (!v) return in.Fail("value");
        using K = PropEvt::Kind;
        const bool num = OneOf(e.name, {pr::kTimePos, pr::kAudioPts, pr::kDuration, pr::kCacheTime, pr::kSpeed});
        const bool boo = OneOf(e.name, {pr::kPause, pr::kPausedForCache, pr::kEofReached});
        if (v->type == JType::Null) {
            e.kind = K::None;
        } else if (num) {
            if (v->type != JType::Number) return in.Fail("value");
            const double lo = e.name == pr::kTimePos || e.name == pr::kAudioPts ? -1e3 : 0;
            if (!(v->n >= lo && v->n <= 1e7)) return in.Fail("value");
            e.kind = K::Number;
            e.number = v->n;
        } else if (boo) {
            if (v->type != JType::Bool) return in.Fail("value");
            e.kind = K::Bool;
            e.boolean = v->b;
        } else if (e.name == pr::kMediaTitle) {
            if (v->type != JType::String || v->s.size() > proto::kMaxTitleBytes * 4) return in.Fail("value");
            e.kind = K::String;
            e.str = CleanText(v->s, proto::kMaxTitleBytes);
        } else if (e.name == pr::kTrackList) {
            e.kind = K::Tracks;
            if (!DecTracks(*v, &e.tracks)) return in.Fail("value");
        } else if (e.name == pr::kVideoParams) {
            if (v->type != JType::Object) return in.Fail("value");
            In vp{*v};
            if (!vp.IntIn("w", 0, 16384, &e.video.w) || !vp.IntIn("h", 0, 16384, &e.video.h) || !vp.NumIn("aspect", 0, 100, &e.video.aspect, true))
                return in.Fail("value");
            e.kind = K::Video;
        } else {
            return in.Fail("unknown property");
        }
        *out = std::move(e);
    } else if (t == m::kEvent) {
        EventEvt e;
        if (!in.IntIn("id", 1, 4294967295.0, &e.id) || !in.IntIn("gen", 0, 4294967295.0, &e.gen) || !in.Str("name", proto::kMaxShortBytes, &e.name) ||
            !OneOf(e.name, {proto::event::kFileLoaded, proto::event::kEndFile}) || !in.Str("reason", proto::kMaxShortBytes, &e.reason, true) ||
            !OneOf(e.reason, {"", "eof", "error", "stop"}))
            return in.Fail("event");
        *out = e;
    } else if (t == m::kError) {
        ErrorEvt e;
        if (!in.IntIn("id", 0, 4294967295.0, &e.id, true) || !in.IntIn("gen", 0, 4294967295.0, &e.gen, true) ||
            !in.IntIn("rid", 0, 4294967295.0, &e.rid, true) || !in.Ih("ih", &e.ih, true) || !in.Str("code", proto::kMaxShortBytes, &e.code) ||
            !in.Str("message", proto::kMaxMessageBytes * 4, &e.message, true))
            return false;
        for (char c : e.code)
            if (!url::IdChar(c) && c != '.') return in.Fail("code");
        e.message = CleanText(std::move(e.message), proto::kMaxMessageBytes);
        *out = e;
    } else if (t == m::kResolved) {
        ResolvedEvt e;
        if (!in.IntIn("rid", 1, 4294967295.0, &e.rid)) return false;
        if (in.o.Get("error")) {
            if (!in.Str("error", proto::kMaxShortBytes, &e.error) || !OneOf(e.error, {proto::err::kYtdlp, proto::err::kNetwork, proto::err::kPrivate, proto::err::kTimeout}) ||
                !in.Str("message", proto::kMaxMessageBytes * 4, &e.message, true))
                return in.Fail("error");
            e.message = CleanText(std::move(e.message), proto::kMaxMessageBytes);
        } else {
            const JValue* items = in.Need("items", JType::Array);
            if (!items || !in.Str("title", proto::kMaxTitleBytes * 4, &e.title, true)) return false;
            e.title = CleanText(std::move(e.title), proto::kMaxTitleBytes);
            for (const JValue& it : items->items) {
                if (e.items.size() >= proto::kMaxResolveItems) break;  // the rest are dropped
                if (it.type != JType::Object) return in.Fail("items");
                In ii{it};
                ResolveItem r;
                if (!ii.Str("id", 11, &r.id) || !url::ValidVideoId(r.id) || !ii.Str("title", proto::kMaxTitleBytes * 4, &r.title, true) ||
                    !ii.NumIn("duration", 0, 1e7, &r.duration, true))
                    continue;  // one bad entry (a deleted video) does not spoil the list
                r.url = url::CanonicalVideoUrl(r.id);
                r.title = CleanText(std::move(r.title), proto::kMaxTitleBytes);
                e.items.push_back(std::move(r));
            }
        }
        *out = std::move(e);
    } else if (t == m::kBtFiles) {
        BtFilesEvt e;
        const JValue* files = nullptr;
        if (!in.Ih("ih", &e.ih) || !in.Str("name", proto::kMaxPathBytes * 4, &e.name, true) || !(files = in.Need("files", JType::Array)))
            return false;
        e.name = CleanText(std::move(e.name), proto::kMaxNameBytes);
        for (const JValue& f : files->items) {
            if (e.files.size() >= proto::kMaxTorrentFiles) break;
            if (f.type != JType::Object) return in.Fail("files");
            In fi{f};
            BtFile b;
            if (!fi.IntIn("index", 0, static_cast<double>(proto::kMaxTorrentFiles - 1), &b.index) ||
                !fi.Str("path", proto::kMaxPathBytes * 4, &b.path) || !fi.NumIn("size", 0, 9007199254740992.0, &b.size) ||
                !fi.Bool("video", &b.video, true))
                return in.Fail("files");
            b.path = CleanText(std::move(b.path), proto::kMaxPathBytes);
            e.files.push_back(std::move(b));
        }
        *out = std::move(e);
    } else if (t == m::kBtStatus) {
        BtStatusEvt e;
        if (!in.Ih("ih", &e.ih) || !in.NumIn("down", 0, 1e12, &e.down, true) || !in.NumIn("up", 0, 1e12, &e.up, true) ||
            !in.IntIn("peers", 0, 100000, &e.peers, true) || !in.NumIn("progress", 0, 1, &e.progress, true) ||
            !in.NumIn("have", 0, 9007199254740992.0, &e.have, true) || !in.Bool("metadata", &e.metadata, true))
            return false;
        *out = e;
    } else if (t == m::kBtRemoved) {
        BtRemovedEvt e;
        if (!in.Ih("ih", &e.ih)) return false;
        *out = e;
    } else if (t == m::kPong) {
        *out = PongEvt{};
    } else {
        return in.Fail("unknown message");
    }
    return true;
}
}  // namespace detail

// Decodes one line (without its '\n') sent in direction `dir`. False with a short reason for anything unusable.
inline bool Decode(std::string_view line, Dir dir, Message* out, std::string* why = nullptr) {
    if (line.size() > proto::kMaxLine) {
        if (why) *why = "line too long";
        return false;
    }
    JValue v;
    if (!ParseJson(line, &v, why)) return false;
    if (v.type != JType::Object) {
        if (why) *why = "not an object";
        return false;
    }
    const JValue* t = v.Get("t");
    if (!t || t->type != JType::String || t->s.size() > proto::kMaxShortBytes) {
        if (why) *why = "no message name";
        return false;
    }
    detail::In in{v};
    const bool ok = dir == Dir::ToHelper ? detail::DecToHelper(t->s, in, out) : detail::DecToCore(t->s, in, out);
    if (!ok && why) *why = std::string(t->s) + ": " + (in.why ? in.why : "invalid");
    return ok;
}

// ---------------------------------------------------------------- framing
// Splits a byte stream into lines. A line longer than kMaxLine is dropped whole (up to its '\n') and counted; the
// buffer never grows past kMaxLine.
class LineSplitter {
public:
    template <class F>
    void Feed(const char* data, size_t n, F&& onLine) {
        for (size_t i = 0; i < n; ++i) {
            const char c = data[i];
            if (c == '\n') {
                if (skipping_) {
                    skipping_ = false;
                } else {
                    if (!buf_.empty() && buf_.back() == '\r') buf_.pop_back();
                    if (!buf_.empty()) onLine(std::string_view(buf_));
                }
                buf_.clear();
            } else if (!skipping_) {
                if (buf_.size() >= proto::kMaxLine) {
                    buf_.clear();
                    skipping_ = true;
                    ++dropped_;
                } else {
                    buf_.push_back(c);
                }
            }
        }
    }
    uint64_t Dropped() const { return dropped_; }
    size_t Pending() const { return buf_.size(); }

private:
    std::string buf_;
    bool skipping_ = false;
    uint64_t dropped_ = 0;
};
}  // namespace melange::media::pipe
