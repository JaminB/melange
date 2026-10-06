#include "music/bank.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

namespace melange::music {
namespace {
// kbit/s by bitrate index 1..14: MPEG-1 Layer III, MPEG-1 Layer II, and MPEG-2 (Layer II and III share one table).
constexpr int kBitrateL3[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
constexpr int kBitrateL2[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
constexpr int kBitrateLsf[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
constexpr int kRate1[3] = {44100, 48000, 32000};
constexpr int kRate2[3] = {22050, 24000, 16000};

struct Frame {
    size_t length = 0;
    uint32_t samples = 0, rate = 0;
    int channels = 0;
};

// Reads the 4-byte header at p; false when it is not a frame we take (MPEG-1/2 Layer II/III, a real bitrate and rate).
bool ParseHeader(const uint8_t* p, Frame* f) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    const int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;     // version 3 = MPEG-1, 2 = MPEG-2; layer 1 = III, 2 = II
    const int bitrateIx = p[2] >> 4, rateIx = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if ((version != 3 && version != 2) || (layer != 1 && layer != 2) || bitrateIx == 0 || bitrateIx == 15 || rateIx == 3) return false;
    const bool mpeg1 = version == 3, l3 = layer == 1;
    const int kbps = !mpeg1 ? kBitrateLsf[bitrateIx] : l3 ? kBitrateL3[bitrateIx] : kBitrateL2[bitrateIx];
    const int rate = mpeg1 ? kRate1[rateIx] : kRate2[rateIx];
    const int coef = (!mpeg1 && l3) ? 72 : 144;
    f->length = static_cast<size_t>(coef * kbps * 1000 / rate + pad);
    f->samples = (!mpeg1 && l3) ? 576 : 1152;
    f->rate = static_cast<uint32_t>(rate);
    f->channels = (p[3] >> 6) == 3 ? 1 : 2;
    return true;
}

void Put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { memcpy(&b[at], &v, 2); }
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { memcpy(&b[at], &v, 4); }
void PutF(std::vector<uint8_t>& b, size_t at, float v) { memcpy(&b[at], &v, 4); }
}  // namespace

Scan ScanMp3(const uint8_t* d, size_t size) {
    Scan s;
    size_t i = 0;
    if (size >= 10 && memcmp(d, "ID3", 3) == 0)
        i = 10 + ((static_cast<size_t>(d[6] & 0x7f) << 21) | (static_cast<size_t>(d[7] & 0x7f) << 14) |
                  (static_cast<size_t>(d[8] & 0x7f) << 7) | static_cast<size_t>(d[9] & 0x7f));
    Track& t = s.track;
    while (i + 4 <= size) {
        Frame f;
        if (ParseHeader(d + i, &f) && i + f.length <= size) {
            if (t.frameCount == 0) {
                t.sampleRate = f.rate;
                t.channels = f.channels;
            } else if (f.rate != t.sampleRate || f.channels != t.channels) {
                s.error = "the sample rate or channel count changes inside the file (" + std::to_string(t.sampleRate) + " Hz " +
                          (t.channels == 1 ? "mono" : "stereo") + ", then " + std::to_string(f.rate) + " Hz " +
                          (f.channels == 1 ? "mono" : "stereo") + ")";
                t = Track{};
                return s;
            }
            t.frames.insert(t.frames.end(), d + i, d + i + f.length);
            ++t.frameCount;
            t.samples += f.samples;
            i += f.length;
            continue;
        }
        ++i;
    }
    if (t.frameCount < kMinFrames) {
        s.error = "only " + std::to_string(t.frameCount) + " MPEG Layer II/III frames found (at least " + std::to_string(kMinFrames) + ")";
        t = Track{};
        return s;
    }
    s.ok = true;
    return s;
}

bool CheckVanillaHeader(const uint8_t* v, size_t size, std::string* err) {
    uint32_t n = 0;
    if (size >= kHeaderBytes) memcpy(&n, v + 4, 4);
    if (size < kHeaderBytes || memcmp(v, "FSB4", 4) != 0 || n != 1) {
        if (err) *err = "not an FSB4 bank with one sample";
        return false;
    }
    return true;
}

bool BuildBank(const uint8_t* vanilla, const std::vector<const Track*>& tracks, std::vector<uint8_t>* out, std::string* err) {
    if (tracks.empty()) {
        if (err) *err = "no tracks";
        return false;
    }
    const uint32_t rate = tracks[0]->sampleRate;
    const int channels = tracks[0]->channels;
    uint64_t samples = 0, bytes = 0;
    for (const Track* t : tracks) {
        if (t->sampleRate != rate || t->channels != channels) {
            if (err) *err = "the tracks differ in sample rate or channel count";
            return false;
        }
        samples += t->samples;
        bytes += t->frames.size();
    }
    if (bytes + 16 + kHeaderBytes + kSampleHeaderBytes > 0x7fffffffu || samples == 0 || samples > 0xffffffffu) {
        if (err) *err = "the bank would be too large";
        return false;
    }
    const uint32_t lengthBytes = static_cast<uint32_t>(bytes), lengthSamples = static_cast<uint32_t>(samples);
    const uint32_t dataSize = (lengthBytes + 15) / 16 * 16;
    std::vector<uint8_t> b(kHeaderBytes + kSampleHeaderBytes + dataSize, 0);
    memcpy(&b[0], "FSB4", 4);
    Put32(b, 4, 1);                                   // numsamples
    Put32(b, 8, static_cast<uint32_t>(kSampleHeaderBytes));
    Put32(b, 12, dataSize);
    memcpy(&b[16], vanilla + 16, 32);                 // version, mode, bank ID, hash
    uint8_t* h = &b[kHeaderBytes];
    Put16(b, kHeaderBytes + 0, static_cast<uint16_t>(kSampleHeaderBytes));
    memcpy(h + 2, "SuddenDeath", 11);                 // char[30], zero padded
    Put32(b, kHeaderBytes + 32, lengthSamples);
    Put32(b, kHeaderBytes + 36, lengthBytes);
    Put32(b, kHeaderBytes + 40, 0);                   // loopstart
    Put32(b, kHeaderBytes + 44, lengthSamples - 1);   // loopend
    Put32(b, kHeaderBytes + 48, 0x00040200);          // mode
    Put32(b, kHeaderBytes + 52, rate);                // deffreq
    Put16(b, kHeaderBytes + 56, 255);                 // defvol
    Put16(b, kHeaderBytes + 58, 128);                 // defpan
    Put16(b, kHeaderBytes + 60, 128);                 // defpri
    Put16(b, kHeaderBytes + 62, static_cast<uint16_t>(channels));
    PutF(b, kHeaderBytes + 64, 1.0f);                 // mindistance
    PutF(b, kHeaderBytes + 68, 10000.0f);             // maxdistance
    Put32(b, kHeaderBytes + 72, 80);                  // varfreq
    Put16(b, kHeaderBytes + 76, 0);                   // varvol
    Put16(b, kHeaderBytes + 78, 0);                   // varpan
    size_t at = kHeaderBytes + kSampleHeaderBytes;
    for (const Track* t : tracks) {
        memcpy(&b[at], t->frames.data(), t->frames.size());
        at += t->frames.size();
    }
    *out = std::move(b);
    return true;
}

std::vector<size_t> ShuffleOrder(size_t n, size_t lastFirst, std::mt19937& rng) {
    std::vector<size_t> o(n);
    for (size_t i = 0; i < n; ++i) o[i] = i;
    if (n < 2) return o;
    // Pick the first among the allowed ones, then shuffle the rest: uniform over the orders that satisfy the rule.
    if (lastFirst < n) {
        size_t k = std::uniform_int_distribution<size_t>(0, n - 2)(rng);
        if (k >= lastFirst) ++k;
        std::swap(o[0], o[k]);
        std::shuffle(o.begin() + 1, o.end(), rng);
    } else {
        std::shuffle(o.begin(), o.end(), rng);
    }
    return o;
}

bool IsSuddenDeathPath(const char* name) {
    if (!name) return false;
    std::string s(name);
    for (char& c : s) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    constexpr std::string_view kTail = "\\audio\\pc\\musuddendeath.fsb";
    return s.size() >= kTail.size() && std::string_view(s).substr(s.size() - kTail.size()) == kTail;
}
}  // namespace melange::music
