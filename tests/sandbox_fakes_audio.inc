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
