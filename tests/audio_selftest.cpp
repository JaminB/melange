// Offline self-test for src/audio: the WAV reader, the per-mod clip cache, the distance falloff and the voice
// bookkeeping. The XAudio2 backend (mixer.cpp) needs a sound device and is not covered here.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/attenuation.h"
#include "audio/clips.h"
#include "audio/voicebook.h"
#include "audio/wav.h"

using namespace melange::audio;

namespace {
int g_fail = 0;
void Expect(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_fail;
    }
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

using Bytes = std::vector<uint8_t>;
void Put16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(Bytes& b, uint32_t v) {
    Put16(b, static_cast<uint16_t>(v));
    Put16(b, static_cast<uint16_t>(v >> 16));
}
void Tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

void Chunk(Bytes& b, const char* id, const Bytes& body) {
    Tag(b, id);
    Put32(b, static_cast<uint32_t>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    if (body.size() & 1) b.push_back(0);
}

Bytes Fmt(uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits) {
    Bytes f;
    Put16(f, tag);
    Put16(f, ch);
    Put32(f, rate);
    Put32(f, rate * ch * (bits / 8));
    Put16(f, static_cast<uint16_t>(ch * (bits / 8)));
    Put16(f, bits);
    return f;
}

// WAVEFORMATEXTENSIBLE: the plain fmt, then cbSize 22, valid bits, channel mask and the subformat GUID.
Bytes FmtExt(uint16_t ch, uint32_t rate, uint16_t bits, uint16_t subTag) {
    Bytes f = Fmt(0xFFFE, ch, rate, bits);
    Put16(f, 22);
    Put16(f, bits);
    Put32(f, ch == 1 ? 4 : 3);
    Put16(f, subTag);
    Put16(f, 0);
    const uint8_t tail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    f.insert(f.end(), tail, tail + 12);
    return f;
}

Bytes Samples(size_t frames, uint16_t ch) {
    Bytes d;
    for (size_t i = 0; i < frames * ch; ++i) Put16(d, static_cast<uint16_t>(i * 37));
    return d;
}

Bytes Riff(const std::vector<std::pair<const char*, Bytes>>& chunks) {
    Bytes body;
    Tag(body, "WAVE");
    for (const auto& c : chunks) Chunk(body, c.first, c.second);
    Bytes b;
    Tag(b, "RIFF");
    Put32(b, static_cast<uint32_t>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

Bytes Wav(uint16_t ch, uint32_t rate, size_t frames) {
    return Riff({{"fmt ", Fmt(1, ch, rate, 16)}, {"data", Samples(frames, ch)}});
}

bool Parse(const Bytes& b, WavInfo* info, std::string* why) { return ParseWav(b.data(), b.size(), info, why); }

void TestWav() {
    WavInfo w;
    std::string why;
    Expect(Parse(Wav(1, 22050, 100), &w, &why), "mono 22.05 kHz accepted");
    Expect(w.channels == 1 && w.rate == 22050 && w.frames == 100 && w.bytes == 200, "mono fields");
    Expect(Parse(Wav(2, 44100, 50), &w, &why), "stereo 44.1 kHz accepted");
    Expect(w.channels == 2 && w.rate == 44100 && w.frames == 50 && w.bytes == 200, "stereo fields");
    Expect(Parse(Wav(1, 8000, 1), &w, &why) && Parse(Wav(2, 48000, 1), &w, &why), "rate range ends accepted");
    Expect(!Parse(Wav(1, 7999, 10), &w, &why) && why.find("rate") != std::string::npos, "7999 Hz rejected");
    Expect(!Parse(Wav(1, 48001, 10), &w, &why), "48001 Hz rejected");
    Expect(!Parse(Wav(3, 22050, 10), &w, &why), "3 channels rejected");

    // The data span points into the input, at the sample bytes.
    const Bytes good = Wav(2, 44100, 4);
    Expect(Parse(good, &w, &why) && w.data >= good.data() && w.data + w.bytes == good.data() + good.size(), "data span is inside the file");

    // Truncated: the data chunk claims more than the file holds.
    Bytes cut = Wav(1, 22050, 100);
    cut.resize(cut.size() - 20);
    Expect(!Parse(cut, &w, &why) && why == "truncated", "truncated data rejected");
    Bytes tiny(good.begin(), good.begin() + 10);
    Expect(!Parse(tiny, &w, &why), "10 bytes rejected");
    Expect(!ParseWav(nullptr, 0, &w, &why), "null rejected");
    Bytes notRiff = good;
    notRiff[0] = 'X';
    Expect(!Parse(notRiff, &w, &why), "no RIFF tag rejected");
    Bytes notWave = good;
    notWave[8] = 'X';
    Expect(!Parse(notWave, &w, &why), "no WAVE tag rejected");

    // Formats.
    Expect(!Parse(Riff({{"fmt ", Fmt(3, 1, 22050, 32)}, {"data", Samples(10, 2)}}), &w, &why) && why == "not PCM", "float format rejected");
    Expect(!Parse(Riff({{"fmt ", Fmt(2, 1, 22050, 4)}, {"data", Samples(10, 1)}}), &w, &why), "ADPCM rejected");
    Expect(!Parse(Riff({{"fmt ", Fmt(1, 1, 22050, 24)}, {"data", Samples(10, 2)}}), &w, &why) && why == "not 16-bit", "24-bit rejected");
    Expect(!Parse(Riff({{"fmt ", Fmt(1, 1, 22050, 8)}, {"data", Samples(10, 1)}}), &w, &why), "8-bit rejected");
    Expect(Parse(Riff({{"fmt ", FmtExt(2, 44100, 16, 1)}, {"data", Samples(10, 2)}}), &w, &why) && w.channels == 2 && w.frames == 10,
           "extensible PCM 16 accepted");
    Expect(!Parse(Riff({{"fmt ", FmtExt(2, 44100, 16, 3)}, {"data", Samples(10, 2)}}), &w, &why), "extensible float rejected");
    Expect(!Parse(Riff({{"fmt ", FmtExt(2, 44100, 24, 1)}, {"data", Samples(10, 2)}}), &w, &why), "extensible 24-bit rejected");

    // Chunk order and padding.
    Expect(!Parse(Riff({{"data", Samples(10, 1)}, {"fmt ", Fmt(1, 1, 22050, 16)}}), &w, &why) && why == "data before fmt", "data before fmt rejected");
    Expect(!Parse(Riff({{"fmt ", Fmt(1, 1, 22050, 16)}}), &w, &why), "no data chunk rejected");
    Expect(!Parse(Riff({{"data", Samples(10, 1)}}), &w, &why), "no fmt chunk rejected");
    Expect(!Parse(Riff({{"fmt ", Fmt(1, 1, 22050, 16)}, {"data", Bytes()}}), &w, &why), "empty data rejected");
    Bytes odd = {1, 2, 3};  // an odd-sized LIST chunk before the data: the pad byte follows it
    Expect(Parse(Riff({{"LIST", odd}, {"fmt ", Fmt(1, 1, 22050, 16)}, {"fact", Bytes{9, 9, 9, 9, 9}}, {"data", Samples(7, 1)}}), &w, &why) &&
               w.frames == 7,
           "odd chunk sizes are padded and unknown chunks skipped");
    // A data chunk with an odd byte count loses the half frame and keeps the rest.
    Bytes oddData = Samples(5, 1);
    oddData.push_back(0x7f);
    Expect(Parse(Riff({{"fmt ", Fmt(1, 1, 22050, 16)}, {"data", oddData}}), &w, &why) && w.frames == 5 && w.bytes == 10, "partial last frame dropped");
    // Stereo with the wrong block alignment.
    Bytes badAlign = Fmt(1, 2, 22050, 16);
    badAlign[12] = 3;
    Expect(!Parse(Riff({{"fmt ", badAlign}, {"data", Samples(4, 2)}}), &w, &why), "wrong block alignment rejected");
    // Trailing bytes after the data chunk are ignored.
    Bytes trailing = Wav(1, 22050, 8);
    trailing.push_back(0);
    trailing.push_back(1);
    Expect(Parse(trailing, &w, &why) && w.frames == 8, "trailing bytes ignored");

    // Size limit: exactly 2 MiB parses, one byte more is "too large".
    Bytes big = Riff({{"fmt ", Fmt(1, 2, 44100, 16)}, {"data", Samples(100, 2)}});
    big.resize(kMaxWavBytes);
    // Patch the data chunk size so the file is valid at the limit: header is 12 + 8 + 16 + 8 bytes.
    const uint32_t dataBytes = static_cast<uint32_t>(kMaxWavBytes - 44);
    big[40] = static_cast<uint8_t>(dataBytes);
    big[41] = static_cast<uint8_t>(dataBytes >> 8);
    big[42] = static_cast<uint8_t>(dataBytes >> 16);
    big[43] = static_cast<uint8_t>(dataBytes >> 24);
    Expect(Parse(big, &w, &why) && w.frames == dataBytes / 4, "2 MiB file accepted");
    big.push_back(0);
    Expect(!Parse(big, &w, &why) && why == "too large", "2 MiB + 1 rejected as too large");
}

void TestClips() {
    ClipCache c(3, 900);
    std::string why;
    const Bytes a = Wav(1, 22050, 100);  // 200 bytes of PCM
    const uint32_t ia = c.Add("a.wav", a.data(), a.size(), &why);
    Expect(ia != 0 && c.Count() == 1 && c.Bytes() == 200, "first clip added");
    Expect(c.Add("a.wav", a.data(), a.size(), &why) == ia && c.Count() == 1 && c.Bytes() == 200, "same key returns the same handle for free");
    Expect(c.Find("a.wav") == ia && c.Find("nope.wav") == 0, "Find");
    auto clip = c.Get(ia);
    Expect(clip && clip->channels == 1 && clip->rate == 22050 && clip->frames == 100 && clip->pcm.size() == 200, "clip data kept");
    Expect(c.Get(ia + 100) == nullptr, "unknown handle");

    const Bytes junk = {1, 2, 3, 4};
    Expect(c.Add("bad.wav", junk.data(), junk.size(), &why) == 0 && !why.empty() && c.Count() == 1, "a bad file adds nothing");

    const Bytes big = Wav(1, 22050, 200);  // 400 bytes
    Expect(c.Add("b.wav", big.data(), big.size(), &why) != 0, "second clip");
    Expect(c.Add("c.wav", big.data(), big.size(), &why) == 0 && why == "sound memory limit reached" && c.Bytes() == 600, "byte cap refuses the third");
    const Bytes small = Wav(1, 22050, 10);
    Expect(c.Add("d.wav", small.data(), small.size(), &why) != 0, "a smaller one still fits");
    Expect(c.Add("e.wav", small.data(), small.size(), &why) == 0 && why == "too many sounds loaded", "count cap refuses the fourth");

    // A voice's reference keeps the PCM alive after the cache is gone.
    std::shared_ptr<const Clip> held;
    {
        ClipCache gone;
        const uint32_t id = gone.Add("x.wav", a.data(), a.size(), &why);
        held = gone.Get(id);
    }
    Expect(held && held->pcm.size() == 200 && held->pcm[0] == a[44], "clip outlives its cache while a voice holds it");

    // The documented limits.
    Expect(kMaxClipsPerMod == 64 && kMaxPcmPerMod == (16u << 20), "default per-mod limits");

    // Equivalent spellings of one file share a key, so they cost one clip.
    Expect(NormalizeClipKey("sfx/boom.wav") == "sfx/boom.wav", "plain key unchanged");
    Expect(NormalizeClipKey("SFX\\Boom.WAV") == "sfx/boom.wav", "case and backslashes folded");
    Expect(NormalizeClipKey("sfx/./boom.wav") == "sfx/boom.wav" && NormalizeClipKey("./sfx//boom.wav") == "sfx/boom.wav", "dot and empty segments dropped");
    Expect(NormalizeClipKey("sfx/.boom.wav") == "sfx/.boom.wav" && NormalizeClipKey("sfx/boom..wav") == "sfx/boom..wav", "dots inside a name are kept");
    Expect(NormalizeClipKey("").empty() && NormalizeClipKey("./").empty(), "empty and dot-only paths give an empty key");
    {
        ClipCache n;
        const uint32_t id = n.Add(NormalizeClipKey("sfx/boom.wav"), a.data(), a.size(), &why);
        Expect(id != 0 && n.Find(NormalizeClipKey("SFX/./Boom.wav")) == id && n.Add(NormalizeClipKey("sfx\\BOOM.wav"), a.data(), a.size(), &why) == id && n.Count() == 1,
               "three spellings of one path are one clip");
    }
}

void TestAttenuation() {
    Expect(Near(Attenuation(0, 150, 1500), 1.0f), "at the listener");
    Expect(Near(Attenuation(150, 150, 1500), 1.0f), "at near");
    Expect(Near(Attenuation(100, 150, 1500), 1.0f), "inside near");
    Expect(Near(Attenuation(825, 150, 1500), 0.5f), "halfway");
    Expect(Near(Attenuation(1500, 150, 1500), 0.0f), "at far");
    Expect(Near(Attenuation(5000, 150, 1500), 0.0f), "beyond far");
    Expect(Near(PositionalVolume(0.8f, 825, 150, 1500), 0.4f), "volume scales the falloff");
    Expect(Near(PositionalVolume(2.0f, 10, 150, 1500), 2.0f), "volume 2 inside near");
    // far <= near: a hard cut at near, never a division by zero or a negative span.
    Expect(Near(Attenuation(100, 200, 200), 1.0f) && Near(Attenuation(200, 200, 200), 1.0f) && Near(Attenuation(201, 200, 200), 0.0f), "far == near is a hard cut");
    Expect(Near(Attenuation(100, 200, 50), 1.0f) && Near(Attenuation(300, 200, 50), 0.0f), "far < near is a hard cut at near");
    Expect(Near(Attenuation(-5, 150, 1500), 1.0f), "negative distance counts as 0");
    Expect(Near(Attenuation(NAN, 150, 1500), 0.0f) && Near(PositionalVolume(1.0f, NAN, 150, 1500), 0.0f), "NaN distance is silent");
    Expect(Near(Attenuation(INFINITY, 150, 1500), 0.0f), "infinite distance is silent");
    const float a[3] = {0, 0, 0}, b[3] = {3, 4, 12};
    Expect(Near(Distance(a, b), 13.0f), "distance");
}

void TestVoiceBook() {
    VoiceBook book(3);
    int modA, modB;
    VoiceInfo va, vb;
    va.owner = &modA;
    vb.owner = &modB;
    const uint32_t i1 = book.Add(va), i2 = book.Add(vb), i3 = book.Add(va);
    Expect(i1 && i2 && i3 && i1 != i2 && i2 != i3, "ids are distinct and non-zero");
    Expect(book.Full() && book.Count() == 3 && book.Add(va) == 0, "full at MaxVoices");
    Expect(book.Owns(&modA, i1) && !book.Owns(&modB, i1) && book.Owns(&modB, i2), "ownership");
    Expect(!book.Owns(&modA, 9999), "unknown id is not owned");
    Expect(book.OwnedBy(&modA).size() == 2 && book.OwnedBy(&modB).size() == 1, "OwnedBy");
    Expect(book.Remove(i1) && !book.Remove(i1) && book.Count() == 2 && !book.Full(), "remove frees a slot once");
    const uint32_t i4 = book.Add(va);
    Expect(i4 > i3, "ids are never reused");
    book.SetMax(1);
    Expect(book.Full() && book.Add(va) == 0, "lowering the max applies to new voices");
    Expect(book.All().size() == 3, "All lists every voice");
    VoiceInfo* f = book.Find(i2);
    Expect(f && f->owner == &modB && book.Find(12345) == nullptr, "Find");

    // A match-end stop spares voices started in the current frame (a sound started from melange.match.end).
    VoiceBook fb(8);
    VoiceInfo old1, old2, fresh;
    old1.startFrame = 90;
    old2.startFrame = 99;
    fresh.startFrame = 100;
    const uint32_t o1 = fb.Add(old1), o2 = fb.Add(old2), f1 = fb.Add(fresh);
    const std::vector<uint32_t> before = fb.StartedBefore(100);
    Expect(before.size() == 2 && before[0] == o1 && before[1] == o2, "StartedBefore returns voices from earlier frames");
    Expect(fb.StartedBefore(101).size() == 3 && fb.StartedBefore(90).empty(), "StartedBefore boundaries");
    Expect(f1 != 0 && fb.Find(f1)->startFrame == 100, "startFrame is kept");
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestWav();
    TestClips();
    TestAttenuation();
    TestVoiceBook();
    if (g_fail) {
        printf("audio_selftest: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("audio_selftest: all passed\n");
    return 0;
}
