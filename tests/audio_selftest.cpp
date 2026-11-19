// Offline self-test for src/audio: the WAV reader, the per-mod clip cache (cut and remove included), the FSB4 speech
// bank reader and decoder, the distance falloff and the voice bookkeeping. The XAudio2 backend (mixer.cpp) needs a sound device and is not covered here.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/attenuation.h"
#include "audio/bank_name.h"
#include "audio/clips.h"
#include "audio/fsb_voice.h"
#include "audio/music_gate.h"
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

// ---- Melange 0.17: cut, remove, and the FSB4 speech banks (synthetic banks only, never game files).
Bytes Pcm16(const std::vector<int16_t>& v) {
    Bytes b;
    for (int16_t s : v) Put16(b, static_cast<uint16_t>(s));
    return b;
}
int16_t At(const Clip& c, size_t i) { return static_cast<int16_t>(c.pcm[2 * i] | c.pcm[2 * i + 1] << 8); }

void TestCutRemove() {
    std::string why;
    ClipCache c(4, 100000);
    std::vector<int16_t> v(8000, 1000);  // 1 s of a constant at 8000 Hz
    const uint32_t src = c.AddPcm("src", 1, 8000, Pcm16(v), &why);
    Expect(src != 0 && c.Bytes() == 16000, "AddPcm stores the PCM");
    Expect(c.AddPcm("src", 1, 8000, Pcm16(v), &why) == src && c.Count() == 1, "AddPcm: the same key, the same handle");
    Expect(c.AddPcm("odd", 2, 8000, Bytes(6, 0), &why) == 0 && why == "unsupported sample format", "AddPcm: a partial stereo frame");
    Expect(c.AddPcm("rate", 1, 4000, Bytes(8, 0), &why) == 0 && why == "unsupported sample format", "AddPcm: a rate below 8000");

    // 0.1 s from 0.25 s: 800 frames; the default fade is 0.008 s = 64 frames at each end.
    const uint32_t cut = c.Cut(src, 0.25, 0.1, kDefaultCutFade, &why);
    auto k = c.Get(cut);
    Expect(cut != 0 && cut != src && k && k->frames == 800 && k->rate == 8000 && k->channels == 1, "cut length");
    Expect(k && At(*k, 0) == 0 && At(*k, 799) == 0, "the cut starts and ends at zero");
    Expect(k && At(*k, 32) == 500 && At(*k, 799 - 32) == 500, "linear fade: half way at half the fade");
    Expect(k && At(*k, 64) == 1000 && At(*k, 400) == 1000 && At(*k, 799 - 64) == 1000, "untouched past the fade");
    Expect(c.Cut(src, 0.25, 0.1, kDefaultCutFade, &why) == cut && c.Count() == 2, "the same cut returns the same handle");
    Expect(c.Cut(src, 0.25001, 0.1, 0.008, &why) == cut, "arguments are quantised to the sample");
    Expect(c.Bytes() == 16000 + 1600, "a cut is a copy and counts");

    const uint32_t tail = c.Cut(src, 0.9, 5.0, 0.0, &why);
    auto t = c.Get(tail);
    Expect(t && t->frames == 800 && At(*t, 0) == 1000 && At(*t, 799) == 1000, "clamped to the end; fade 0 is none");
    Expect(c.Cut(src, 1.0, 1.0, 0.0, &why) == 0 && why == "empty cut", "nothing left past the end");
    Expect(c.Cut(src, 0.5, 0.0, 0.0, &why) == 0 && why == "empty cut", "zero length");
    Expect(c.Cut(src, 0.5, -1.0, 0.0, &why) == 0 && why == "empty cut", "negative length");
    Expect(c.Cut(src, 0.5, NAN, 0.0, &why) == 0 && why == "empty cut", "NaN length");
    Expect(c.Cut(999, 0, 1, 0, &why) == 0 && why == "unknown sound", "unknown source");
    {
        // From before the start clamps to 0; a fade longer than half the cut is cut to half: 4 frames, fade 2.
        ClipCache s;
        const uint32_t id = s.AddPcm("s", 1, 8000, Pcm16({800, 800, 800, 800, 800, 800}), &why);
        const uint32_t h = s.Cut(id, -3.0, 4.0 / 8000, 0.25, &why);
        auto x = s.Get(h);
        Expect(x && x->frames == 4 && At(*x, 0) == 0 && At(*x, 1) == 400 && At(*x, 2) == 400 && At(*x, 3) == 0,
               "fade clamped to half the cut, start clamped to 0");
        const uint32_t n = s.Cut(id, 0, 4.0 / 8000, NAN, &why);
        Expect(n != 0, "a NaN fade is the default");
        // Stereo: both channels of a frame get the same gain.
        const uint32_t st = s.AddPcm("st", 2, 8000, Pcm16({100, -100, 100, -100, 100, -100, 100, -100}), &why);
        auto y = s.Get(s.Cut(st, 0, 1, 1.0 / 8000, &why));
        Expect(y && y->frames == 4 && At(*y, 0) == 0 && At(*y, 1) == 0 && At(*y, 2) == 100 && At(*y, 3) == -100 && At(*y, 7) == 0,
               "stereo cut fades both channels");
    }

    // Limits: the fourth clip fits, the fifth does not; the byte cap applies to cuts too.
    Expect(c.Count() == 3 && c.Cut(src, 0, 0.01, 0, &why) != 0 && c.Count() == 4, "fourth clip");
    Expect(c.Cut(src, 0, 0.02, 0, &why) == 0 && why == "too many sounds loaded", "count cap applies to cuts");

    // Remove frees the slot and the bytes; a held reference survives; the key is free again.
    auto held = c.Get(cut);
    const size_t before = c.Bytes();
    Expect(c.Remove(cut) && c.Count() == 3 && c.Bytes() == before - 1600, "Remove frees the slot and the memory");
    Expect(!c.Get(cut) && !c.Remove(cut), "a removed handle is gone");
    Expect(held && held->frames == 800 && At(*held, 400) == 1000, "a playing voice's reference outlives Remove");
    const uint32_t again = c.Cut(src, 0.25, 0.1, kDefaultCutFade, &why);
    Expect(again != 0 && again != cut, "the same cut after Remove is a new handle (ids are never reused)");
    Expect(c.Remove(src) && c.Cut(src, 0, 0.1, 0, &why) == 0 && why == "unknown sound", "a removed source cannot be cut");
    Expect(c.Get(again) && c.Get(again)->frames == 800, "a cut outlives its source");
    ClipCache tiny(64, 1000);
    Expect(tiny.AddPcm("big", 1, 8000, Bytes(1002, 0), &why) == 0 && why == "sound memory limit reached", "AddPcm: byte cap");
}

// One FSB4 sample for Bank().
struct FsbSample {
    std::string name;
    uint32_t frames, mode, rate;
    uint16_t channels;
    Bytes data;
};

// A synthetic FSB4 bank laid out like the game's: 80-byte sample headers, each sample's data on a 32-byte boundary.
Bytes Bank(const std::vector<FsbSample>& ss, uint32_t flags = 0x40, uint16_t hdr = 80) {
    const size_t headers = ss.size() * hdr;
    Bytes data;
    for (const auto& s : ss) {
        while ((48 + headers + data.size()) % 32) data.push_back(0xEE);  // padding that must be skipped
        data.insert(data.end(), s.data.begin(), s.data.end());
    }
    Bytes b;
    Tag(b, "FSB4");
    Put32(b, static_cast<uint32_t>(ss.size()));
    Put32(b, static_cast<uint32_t>(headers));
    Put32(b, static_cast<uint32_t>(data.size()));
    Put32(b, 0x40000);
    Put32(b, flags);
    b.resize(48, 0);
    for (const auto& s : ss) {
        const size_t at = b.size();
        b.resize(at + hdr, 0);
        b[at] = static_cast<uint8_t>(hdr);
        if (hdr >= 32) memcpy(&b[at + 2], s.name.data(), s.name.size() < 30 ? s.name.size() : 30);
        auto put = [&](size_t off, uint32_t v) {
            for (size_t i = 0; i < 4 && at + off + i < b.size(); ++i) b[at + off + i] = static_cast<uint8_t>(v >> (8 * i));
        };
        put(32, s.frames);
        put(36, static_cast<uint32_t>(s.data.size()));
        put(44, s.frames ? s.frames - 1 : 0);
        put(48, s.mode);
        put(52, s.rate);
        if (hdr > 62) {
            b[at + 56] = 255;
            b[at + 62] = static_cast<uint8_t>(s.channels);
        }
    }
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

// Silent MPEG-1 Layer III frames: 128 kbit/s, 44100 Hz, mono, no CRC: 417 bytes, all side info zero (no reservoir).
Bytes Mp3(int frames) {
    Bytes b;
    for (int f = 0; f < frames; ++f) {
        const uint8_t h[4] = {0xFF, 0xFB, 0x90, 0xC0};
        b.insert(b.end(), h, h + 4);
        b.resize(b.size() + 413, 0);
    }
    return b;
}

void TestFsb() {
    std::string why;
    std::vector<int16_t> ramp;
    for (int i = 0; i < 100; ++i) ramp.push_back(static_cast<int16_t>(i * 300 - 15000));
    const std::vector<FsbSample> ss = {
        {"Nooo_01", 100, 0x10, 32000, 1, Pcm16(ramp)},
        {"taunt 2", 3000, 0x40200, 44100, 1, Mp3(3)},       // 3 x 1152 decoded, cut to the header's 3000
        {"ADPCM", 64, 0x400000, 22050, 1, Bytes(40, 0x11)},  // listed, not decodable
        {"nooo_01", 3, 0x10, 32000, 1, Pcm16({7, 8, 9})},    // the same name in another case
        {"AbCdEfGhIjKlMnOpQrStUvWxYz0123", 2, 0x50, 32000, 2, Pcm16({1, 2, 3, 4})},  // a 30-char name, no NUL; stereo
    };
    const Bytes b = Bank(ss);
    size_t need = 0;
    Expect(fsb::HeaderBytes(b.data(), b.size(), &need, &why) && need == 48 + 5 * 80, "header bytes");
    fsb::Bank bank;
    Expect(fsb::ParseBank(b.data(), need, b.size(), &bank, &why) && bank.samples.size() == 5, "parses from the header bytes alone");
    if (bank.samples.size() == 5) {
        const auto& s0 = bank.samples[0];
        Expect(s0.name == "Nooo_01" && s0.frames == 100 && s0.rate == 32000 && s0.channels == 1 && s0.codec == fsb::Codec::Pcm16,
               "PCM sample header");
        Expect(std::fabs(s0.Seconds() - 100.0 / 32000) < 1e-9, "seconds from frames / rate");
        Expect(bank.samples[1].name == "taunt 2" && bank.samples[1].codec == fsb::Codec::Mpeg, "MPEG sample, a name with a space");
        Expect(bank.samples[2].codec == fsb::Codec::Other, "another codec is listed as Other");
        Expect(bank.samples[4].name.size() == 30 && bank.samples[4].channels == 2, "a 30-character name without NUL");
        bool aligned = true;
        for (const auto& s : bank.samples) aligned = aligned && s.offset % 32 == 0;
        Expect(aligned && s0.offset == ((48 + 400 + 31) & ~31u), "every sample starts on a 32-byte boundary");
        Expect(bank.samples[1].offset == ((s0.offset + 200 + 31) & ~31ull), "the next sample after the padding");
        Expect(fsb::FindLine(bank, "Nooo_01") == 0 && fsb::FindLine(bank, "nooo_01") == 3 && fsb::FindLine(bank, "NOOO_01") == 0,
               "exact match first, then case-insensitive");
        Expect(fsb::FindLine(bank, "TAUNT 2") == 1 && fsb::FindLine(bank, "taunt") == -1 && fsb::FindLine(bank, "") == -1, "FindLine misses");

        fsb::Pcm pcm;
        Expect(fsb::DecodeSample(s0, b.data() + static_cast<size_t>(s0.offset), s0.bytes, &pcm, &why) && pcm.channels == 1 && pcm.rate == 32000 &&
                   pcm.frames == 100 && pcm.samples.size() == 100 && pcm.samples[0] == -15000 && pcm.samples[99] == 14700,
               "PCM16 decodes verbatim");
        const auto& s1 = bank.samples[1];
        Expect(fsb::DecodeSample(s1, b.data() + static_cast<size_t>(s1.offset), s1.bytes, &pcm, &why), ("MPEG decodes: " + why).c_str());
        Expect(pcm.channels == 1 && pcm.rate == 44100 && pcm.frames == 3000 && pcm.samples.size() == 3000, "MPEG cut to the header length");
        bool silent = true;
        for (int16_t x : pcm.samples) silent = silent && x == 0;
        Expect(silent, "silent frames decode to silence");
        fsb::Sample longer = s1;
        longer.frames = 5000;
        Expect(fsb::DecodeSample(longer, b.data() + static_cast<size_t>(s1.offset), s1.bytes, &pcm, &why) && pcm.frames == 3456, "MPEG: what the frames hold");
        Expect(!fsb::DecodeSample(s1, b.data() + static_cast<size_t>(s1.offset), 3, &pcm, &why) && why == "undecodable sample", "MPEG: no whole frame");
        const Bytes junk(500, 0x5A);
        Expect(!fsb::DecodeSample(s1, junk.data(), junk.size(), &pcm, &why) && why == "undecodable sample", "MPEG: not MPEG data");
        const auto& s2 = bank.samples[2];
        Expect(!fsb::DecodeSample(s2, b.data() + static_cast<size_t>(s2.offset), s2.bytes, &pcm, &why) && why == "unsupported sample format", "other codecs refused");
        const auto& s4 = bank.samples[4];
        Expect(fsb::DecodeSample(s4, b.data() + static_cast<size_t>(s4.offset), s4.bytes, &pcm, &why) && pcm.channels == 2 && pcm.frames == 2 && pcm.samples[3] == 4,
               "stereo PCM16");
        Expect(!fsb::DecodeSample(s0, b.data() + static_cast<size_t>(s0.offset), 1, &pcm, &why), "PCM: less than one frame");
    }

    // Refused: bad magic, truncation, a sample beyond the file, zero samples, basic headers, short headers.
    Bytes bad = b;
    bad[3] = '3';
    Expect(!fsb::HeaderBytes(bad.data(), bad.size(), &need, &why) && why == "not an FSB4 bank", "FSB3 refused");
    Expect(!fsb::HeaderBytes(b.data(), 47, &need, &why), "shorter than the header");
    Expect(!fsb::ParseBank(b.data(), 48 + 399, b.size(), &bank, &why) && why == "truncated bank", "sample headers cut short");
    Expect(!fsb::ParseBank(b.data(), b.size(), b.size() - 1, &bank, &why) && why == "truncated bank", "the last sample's data cut short");
    const Bytes none = Bank({});
    Expect(!fsb::HeaderBytes(none.data(), none.size(), &need, &why), "no samples");
    const Bytes basic = Bank(ss, 0x42);
    Expect(!fsb::ParseBank(basic.data(), basic.size(), basic.size(), &bank, &why) && why == "unsupported bank", "basic headers refused");
    const Bytes shortHdr = Bank({ss[0]}, 0x40, 40);
    Expect(!fsb::HeaderBytes(shortHdr.data(), shortHdr.size(), &need, &why), "sample headers too small");
    Bytes huge = b;
    huge[4] = 0xFF;
    huge[5] = 0xFF;
    Expect(!fsb::HeaderBytes(huge.data(), huge.size(), &need, &why), "an absurd sample count");
    Bytes lying = b;
    lying[48 + 80 + 36 + 2] = 0x10;  // the MPEG sample claims 1 MiB more data
    Expect(!fsb::ParseBank(lying.data(), lying.size(), lying.size(), &bank, &why) && why == "truncated bank", "data size past the file");
    Bytes zsize = b;
    zsize[48 + 80] = 0;  // the second sample header's size
    Expect(!fsb::ParseBank(zsize.data(), zsize.size(), zsize.size(), &bank, &why) && why == "bad sample header", "a zero header size");

    // Bank names.
    Expect(fsb::ValidBankName("voclassi") && fsb::ValidBankName("voRussian") && fsb::ValidBankName("VO_x-1"), "valid bank names");
    Expect(!fsb::ValidBankName("") && !fsb::ValidBankName("classi") && !fsb::ValidBankName("vo/../x") &&
               !fsb::ValidBankName("voclassi.fsb") && !fsb::ValidBankName("vo classi") && !fsb::ValidBankName(std::string(33, 'v')),
           "invalid bank names");
    Expect(fsb::ValidBankName("vo" + std::string(30, 'x')), "32 characters is the most");
    Expect(fsb::NormalizeBankName("voclassi") == "voclassi" && fsb::NormalizeBankName("Speech/voclassi") == "voclassi" &&
               fsb::NormalizeBankName("Data\\Audio\\PC\\voRussian.fsb") == "voRussian" && fsb::NormalizeBankName("a/b.c/vocad") == "vocad",
           "SfxBankName normalised");
    Expect(fsb::NormalizeBankName("").empty() && fsb::NormalizeBankName("Speech/").empty() && fsb::NormalizeBankName("music").empty() &&
               fsb::NormalizeBankName("vo\xC3\xA9").empty(),
           "an unusable SfxBankName is empty");
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

void TestMusicGate() {
    int modA, modB;
    MusicGate g;
    Expect(!g.Wanted() && Near(g.Level(), 1.0f) && !g.Fading() && !g.Silent(), "starts audible");
    g.Step(10.0);
    Expect(Near(g.Level(), 1.0f), "idle steps change nothing");

    // OR across owners, one boolean per owner.
    g.Set(&modA, true, 0.4f);
    Expect(g.Wanted() && g.Has(&modA) && !g.Has(&modB), "an owner asks");
    g.Set(&modA, true, 0.4f);
    g.Set(&modB, true, 0.4f);
    Expect(g.Wanted() && g.Has(&modB), "a second owner");
    g.Release(&modA);
    Expect(g.Wanted() && !g.Has(&modA), "still wanted while another owner asks");
    g.Set(&modB, false);
    Expect(!g.Wanted(), "the last release");
    g.Release(&modB);
    Expect(!g.Wanted(), "releasing an owner that never asked is harmless");

    // The fade: 0.4 s from audible to silent.
    MusicGate f;
    double t = 100.0;
    f.Step(t);
    f.Set(&modA, true, 0.4f);
    f.Step(t);
    Expect(Near(f.Level(), 1.0f), "no time passed, no movement");
    f.Step(t += 0.1);
    Expect(Near(f.Level(), 0.75f) && f.Fading(), "a quarter of the fade");
    f.Step(t += 0.2);
    Expect(Near(f.Level(), 0.25f), "three quarters");
    f.Step(t += 0.1);
    Expect(f.Silent() && !f.Fading(), "silent at the end");
    f.Step(t += 5.0);
    Expect(f.Silent(), "stays silent");
    // Fading back in is the reverse, and a reversal in mid-fade turns around from where it is.
    f.Set(&modA, false, 0.4f);
    f.Step(t += 0.1);
    Expect(Near(f.Level(), 0.25f), "fade in");
    f.Set(&modA, true, 0.4f);
    f.Step(t += 0.05);
    Expect(Near(f.Level(), 0.125f), "a reversal turns around from the current level");
    f.Release(&modA);  // a released owner fades the music back
    f.Step(t += 0.2);
    f.Step(t += 0.2);
    f.Step(t += 0.2);
    Expect(Near(f.Level(), 1.0f), "release restores the music");

    // fade = 0 is at once; negative or NaN is the default; a long gap never moves more than kMaxStep.
    MusicGate z;
    z.Set(&modA, true, 0.0f);
    z.Step(1.0);
    Expect(z.Silent(), "fade 0 mutes at once");
    z.Set(&modA, false, 0.0f);
    z.Step(1.0);
    Expect(Near(z.Level(), 1.0f), "fade 0 restores at once");
    MusicGate d;
    d.Step(0.0);
    d.Set(&modA, true, NAN);
    d.Step(0.1);
    Expect(Near(d.Level(), 0.75f), "a NaN fade is the 0.4 s default");
    d.Set(&modA, true, -3.0f);
    d.Step(0.3);
    Expect(Near(d.Level(), 0.25f), "a negative fade is the default too");
    MusicGate gap;
    gap.Step(0.0);
    gap.Set(&modA, true, 10.0f);
    gap.Step(1000.0);
    Expect(gap.Level() > 0.9f, "a long gap between frames moves at most kMaxStep");
    gap.Step(900.0);
    Expect(gap.Level() > 0.9f, "a clock that goes backwards does not move the level");
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestWav();
    TestClips();
    TestCutRemove();
    TestFsb();
    TestAttenuation();
    TestVoiceBook();
    TestMusicGate();
    if (g_fail) {
        printf("audio_selftest: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("audio_selftest: all passed\n");
    return 0;
}
