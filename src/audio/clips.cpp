#include "audio/clips.h"


#include "audio/wav.h"

namespace melange::audio {
std::string NormalizeClipKey(const std::string& rel) {
    std::string out, seg;
    auto flush = [&] {
        if (!seg.empty() && seg != ".") {
            if (!out.empty()) out += '/';
            out += seg;
        }
        seg.clear();
    };
    for (char c : rel) {
        if (c == '/' || c == '\\') flush();
        else seg += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    flush();
    return out;
}

uint32_t ClipCache::Find(const std::string& key) const {
    auto it = byKey_.find(key);
    return it == byKey_.end() ? 0 : it->second;
}

uint32_t ClipCache::Add(const std::string& key, const uint8_t* bytes, size_t n, std::string* why) {
    if (const uint32_t have = Find(key)) return have;
    if (clips_.size() >= maxClips_) {
        if (why) *why = "too many sounds loaded";
        return 0;
    }
    WavInfo info;
    if (!ParseWav(bytes, n, &info, why)) return 0;
    if (bytes_ + info.bytes > maxBytes_) {
        if (why) *why = "sound memory limit reached";
        return 0;
    }
    auto c = std::make_shared<Clip>();
    c->id = ++next_;
    c->channels = info.channels;
    c->rate = info.rate;
    c->frames = info.frames;
    c->pcm.assign(info.data, info.data + info.bytes);
    bytes_ += info.bytes;
    byKey_[key] = c->id;
    clips_[c->id] = std::move(c);
    return next_;
}

std::shared_ptr<const Clip> ClipCache::Get(uint32_t id) const {
    auto it = clips_.find(id);
    return it == clips_.end() ? nullptr : it->second;
}
}  // namespace melange::audio
