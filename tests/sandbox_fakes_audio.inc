// sandbox_selftest fakes for wum.audio.music (owner: C1): stand-ins for the music gate. Included inside namespace
// melange, after the other fakes.
namespace fakemusic {
bool g_musicAllow = true;  // [Audio] AllowMusicSuppress
bool g_musicDead = false;  // FMOD cannot be driven
bool g_musicLive = true;   // the music category was found
float g_musicFade = -2.0f; // the fade of the last accepted request
// owner -> g_audioReleases when it asked. The Audio fake above does not tell who was released; a release since the
// request counts as this owner's release (the cases keep one requesting mod alive at a time around unloads).
std::map<const void*, int> g_musicReq;
}  // namespace fakemusic
namespace audio {
bool MusicAvailable() { return fakemusic::g_musicLive; }
const char* MusicSuppress(const void* owner, bool on, float fade) {
    if (!fakemusic::g_musicAllow) return "disabled";
    if (fakemusic::g_musicDead) return "unavailable";
    fakemusic::g_musicFade = fade;
    if (on) fakemusic::g_musicReq[owner] = ::fake::g_audioReleases;
    else fakemusic::g_musicReq.erase(owner);
    return nullptr;
}
void MusicState(const void* owner, bool* effective, bool* mine) {
    for (auto it = fakemusic::g_musicReq.begin(); it != fakemusic::g_musicReq.end();)
        it = it->second != ::fake::g_audioReleases ? fakemusic::g_musicReq.erase(it) : std::next(it);
    *effective = !fakemusic::g_musicReq.empty();
    *mine = fakemusic::g_musicReq.count(owner) != 0;
}
float MusicLevel() { return 1.0f; }
}  // namespace audio

// Melange 0.17: cut, unload, set and wum.audio.voice. Clips go through the real cache; the speech banks are a fake
// "vofake" bank with two lines and a "vobroken" bank that cannot be read.
namespace fakevoice {
struct Line {
    const char* name;
    double seconds;
};
const Line kLines[] = {{"Nooo_01", 0.5}, {"taunt 2", 0.25}};
std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
bool Bank(const std::string& bank, std::string* why) {
    const std::string b = Lower(bank);
    if (b == "vofake") return true;
    *why = b == "vobroken" ? "unreadable bank" : "unknown bank";
    return false;
}
}  // namespace fakevoice
namespace audio {
uint32_t CutClip(const void* owner, uint32_t clip, double from, double len, double fade, std::string* why) {
    auto it = ::fake::g_audioClips.find(owner);
    if (it == ::fake::g_audioClips.end()) {
        *why = "unknown sound";
        return 0;
    }
    return it->second.Cut(clip, from, len, fade, why);
}
bool UnloadClip(const void* owner, uint32_t clip) {
    auto it = ::fake::g_audioClips.find(owner);
    return it != ::fake::g_audioClips.end() && it->second.Remove(clip);
}
bool SetVoice(const void* owner, uint32_t voice, const float* volume, const float* pitch) {
    auto it = ::fake::g_audioVoices.find(voice);
    if (it == ::fake::g_audioVoices.end() || it->second.owner != owner) return false;
    if (volume) it->second.opts.volume = *volume;
    if (pitch) it->second.opts.pitch = *pitch;
    return true;
}
bool VoiceLines(const std::string& bank, std::vector<VoiceLine>* out, std::string* why) {
    if (!fakevoice::Bank(bank, why)) return false;
    out->clear();
    for (const auto& l : fakevoice::kLines) out->push_back({l.name, l.seconds});
    return true;
}
uint32_t LoadVoice(const void* owner, const std::string& bank, const std::string& line, std::string* why) {
    if (!fakevoice::Bank(bank, why)) return 0;
    const fakevoice::Line* hit = nullptr;
    for (const auto& l : fakevoice::kLines)
        if (line == l.name) hit = &l;
    for (const auto& l : fakevoice::kLines)
        if (!hit && fakevoice::Lower(line) == fakevoice::Lower(l.name)) hit = &l;
    if (!hit) {
        *why = "unknown line";
        return 0;
    }
    const size_t frames = static_cast<size_t>(hit->seconds * 8000);
    return ::fake::g_audioClips[owner].AddPcm("voice:vofake/" + std::string(hit->name), 1, 8000,
                                               std::vector<uint8_t>(frames * 2, 0x20), why);
}
}  // namespace audio
