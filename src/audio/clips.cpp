#include "audio/clips.h"

#include <cmath>
#include <cstdio>

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
    return AddPcm(key, info.channels, info.rate, std::vector<uint8_t>(info.data, info.data + info.bytes), why);
}

uint32_t ClipCache::AddPcm(const std::string& key, uint16_t channels, uint32_t rate, std::vector<uint8_t> pcm,
                           std::string* why) {
    if (const uint32_t have = Find(key)) return have;
    if (channels < 1 || channels > 2 || rate < kMinRate || rate > kMaxRate || pcm.empty() || pcm.size() % (2u * channels)) {
        if (why) *why = "unsupported sample format";
        return 0;
    }
    if (clips_.size() >= maxClips_) {
        if (why) *why = "too many sounds loaded";
        return 0;
    }
    if (pcm.size() > maxBytes_ || bytes_ + pcm.size() > maxBytes_) {
        if (why) *why = "sound memory limit reached";
        return 0;
    }
    auto c = std::make_shared<Clip>();
    c->id = ++next_;
    c->channels = channels;
    c->rate = rate;
    c->frames = static_cast<uint32_t>(pcm.size() / (2u * channels));
    c->pcm = std::move(pcm);
    bytes_ += c->pcm.size();
    byKey_[key] = c->id;
    clips_[c->id] = std::move(c);
    return next_;
}

uint32_t ClipCache::Cut(uint32_t src, double from, double len, double fade, std::string* why) {
    const std::shared_ptr<const Clip> s = Get(src);
    if (!s) {
        if (why) *why = "unknown sound";
        return 0;
    }
    // In frames, clamped to the clip. NaN start = 0; a NaN or non-positive length leaves nothing.
    const double rate = s->rate;
    double f0 = std::isnan(from) ? 0.0 : std::floor(from * rate + 0.5);
    f0 = f0 < 0 ? 0 : (f0 > s->frames ? s->frames : f0);
    const uint32_t start = static_cast<uint32_t>(f0);
    const uint32_t room = s->frames - start;
    double fl = std::isnan(len) ? 0.0 : std::floor(len * rate + 0.5);
    fl = fl < 0 ? 0 : (fl > room ? room : fl);
    const uint32_t frames = static_cast<uint32_t>(fl);
    if (frames == 0) {
        if (why) *why = "empty cut";
        return 0;
    }
    double fd = std::isnan(fade) ? kDefaultCutFade : fade;
    fd = fd < 0 ? 0 : (fd > kMaxCutFade ? kMaxCutFade : fd);
    uint32_t fadeFrames = static_cast<uint32_t>(std::floor(fd * rate + 0.5));
    if (fadeFrames > frames / 2) fadeFrames = frames / 2;

    char key[64];
    snprintf(key, sizeof key, "cut:%u:%u:%u:%u", src, start, frames, fadeFrames);
    if (const uint32_t have = Find(key)) return have;
    const size_t ch = s->channels;
    const size_t bytes = static_cast<size_t>(frames) * ch * 2;
    if (clips_.size() >= maxClips_) {
        if (why) *why = "too many sounds loaded";
        return 0;
    }
    if (bytes_ + bytes > maxBytes_) {
        if (why) *why = "sound memory limit reached";
        return 0;
    }
    std::vector<uint8_t> pcm(s->pcm.begin() + static_cast<ptrdiff_t>(start * ch * 2),
                             s->pcm.begin() + static_cast<ptrdiff_t>(start * ch * 2 + bytes));
    // Frame i of the fade-in is scaled by i / fadeFrames, and the fade-out mirrors it, so the cut starts and ends at 0.
    auto scale = [&](uint32_t frame, float g) {
        for (size_t c = 0; c < ch; ++c) {
            uint8_t* p = pcm.data() + (frame * ch + c) * 2;
            const int16_t v = static_cast<int16_t>(p[0] | p[1] << 8);
            const int16_t o = static_cast<int16_t>(std::lround(v * g));
            p[0] = static_cast<uint8_t>(o);
            p[1] = static_cast<uint8_t>(static_cast<uint16_t>(o) >> 8);
        }
    };
    for (uint32_t i = 0; i < fadeFrames; ++i) {
        const float g = static_cast<float>(i) / static_cast<float>(fadeFrames);
        scale(i, g);
        scale(frames - 1 - i, g);
    }
    return AddPcm(key, s->channels, s->rate, std::move(pcm), why);
}

bool ClipCache::Remove(uint32_t id) {
    auto it = clips_.find(id);
    if (it == clips_.end()) return false;
    bytes_ -= it->second->pcm.size();
    clips_.erase(it);
    for (auto k = byKey_.begin(); k != byKey_.end(); ++k)
        if (k->second == id) {
            byKey_.erase(k);
            break;
        }
    return true;
}

std::shared_ptr<const Clip> ClipCache::Get(uint32_t id) const {
    auto it = clips_.find(id);
    return it == clips_.end() ? nullptr : it->second;
}
}  // namespace melange::audio
