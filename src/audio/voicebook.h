#pragma once
// Which voices exist, who owns them and where the positional ones are. Pure bookkeeping: the XAudio2 side is mixer.cpp,
// which is told the same ids. Ids are never reused within a run, so a stale id from a mod can never stop a newer voice.
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace melange::audio {
struct VoiceInfo {
    const void* owner = nullptr;  // the mod generation that started it
    bool positional = false;
    float pos[3] = {0, 0, 0};
    float volume = 1.0f;          // the mod's requested volume, before distance
    float applied = -1.0f;        // the level last handed to the mixer
    uint64_t startFrame = 0;      // events::FrameCount() when it started
};

class VoiceBook {
public:
    explicit VoiceBook(size_t maxVoices = 32) : max_(maxVoices) {}
    void SetMax(size_t m) { max_ = m; }
    size_t Max() const { return max_; }
    size_t Count() const { return voices_.size(); }
    bool Full() const { return voices_.size() >= max_; }

    // 0 when the book is full.
    uint32_t Add(const VoiceInfo& v) {
        if (Full()) return 0;
        const uint32_t id = ++next_;
        voices_[id] = v;
        return id;
    }
    bool Remove(uint32_t id) { return voices_.erase(id) != 0; }
    bool Owns(const void* owner, uint32_t id) const {
        auto it = voices_.find(id);
        return it != voices_.end() && it->second.owner == owner;
    }
    VoiceInfo* Find(uint32_t id) {
        auto it = voices_.find(id);
        return it == voices_.end() ? nullptr : &it->second;
    }
    std::vector<uint32_t> OwnedBy(const void* owner) const {
        std::vector<uint32_t> ids;
        for (const auto& [id, v] : voices_)
            if (v.owner == owner) ids.push_back(id);
        return ids;
    }
    // Voices started before `frame`. A match-end stop passes the current frame, so a sound a mod starts from its
    // melange.match.end handler (which runs in the same frame, before Audio's own upkeep) is not cut off at birth.
    std::vector<uint32_t> StartedBefore(uint64_t frame) const {
        std::vector<uint32_t> ids;
        for (const auto& [id, v] : voices_)
            if (v.startFrame < frame) ids.push_back(id);
        return ids;
    }
    std::vector<uint32_t> All() const {
        std::vector<uint32_t> ids;
        for (const auto& [id, v] : voices_) ids.push_back(id);
        return ids;
    }
    std::map<uint32_t, VoiceInfo>& Voices() { return voices_; }

private:
    size_t max_;
    uint32_t next_ = 0;
    std::map<uint32_t, VoiceInfo> voices_;
};
}  // namespace melange::audio
