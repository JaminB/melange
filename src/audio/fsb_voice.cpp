#include "audio/fsb_voice.h"

#include <cstring>

// minimp3 (CC0, header-only, pinned in CMakeLists.txt). Its own code is not held to this project's warning level.
#pragma warning(push, 0)
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"
#pragma warning(pop)

namespace melange::audio::fsb {
namespace {
constexpr uint32_t kFlagBasicHeaders = 0x2;  // later samples carry an 8-byte header only: not used by the game
constexpr uint32_t kFlagBigEndianPcm = 0x8;
constexpr uint32_t kMode8Bits = 0x8, kMode16Bits = 0x10, kModeStereo = 0x40, kModeMpeg = 0x200;
// Other codecs (IMA ADPCM, VAG, XMA, GameCube ADPCM, multichannel, CELT...): listed but never decoded.
constexpr uint32_t kModeOtherCodecs = 0x00400000 | 0x00800000 | 0x01000000 | 0x02000000 | 0x04000000 | 0x08000000 | 0x1000;
constexpr size_t kSampleHeaderMin = 64;  // through the channel count

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

bool Fail(std::string* why, const char* w) {
    if (why) *why = w;
    return false;
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
}  // namespace

bool HeaderBytes(const uint8_t* p, size_t n, size_t* need, std::string* why) {
    if (n < kHeaderBytes || memcmp(p, "FSB4", 4) != 0) return Fail(why, "not an FSB4 bank");
    const uint32_t count = U32(p + 4), shb = U32(p + 8);
    if (count == 0 || count > kMaxSamples || shb > kMaxSampleHeaderBytes || shb < count * kSampleHeaderMin)
        return Fail(why, "bad bank header");
    *need = kHeaderBytes + shb;
    return true;
}

bool ParseBank(const uint8_t* p, size_t n, uint64_t fileBytes, Bank* out, std::string* why) {
    size_t need = 0;
    if (!HeaderBytes(p, n, &need, why)) return false;
    if (n < need || fileBytes < need) return Fail(why, "truncated bank");
    const uint32_t count = U32(p + 4), dataBytes = U32(p + 12);
    Bank b;
    b.flags = U32(p + 20);
    if (b.flags & kFlagBasicHeaders) return Fail(why, "unsupported bank");
    if (static_cast<uint64_t>(need) + dataBytes > fileBytes) return Fail(why, "truncated bank");
    b.samples.reserve(count);
    size_t at = kHeaderBytes;
    uint64_t data = need;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + kSampleHeaderMin > need) return Fail(why, "truncated bank");
        const uint8_t* h = p + at;
        const uint16_t size = U16(h);
        if (size < kSampleHeaderMin || at + size > need) return Fail(why, "bad sample header");
        Sample s;
        const char* nm = reinterpret_cast<const char*>(h + 2);
        s.name.assign(nm, strnlen(nm, 30));
        s.frames = U32(h + 32);
        s.bytes = U32(h + 36);
        s.mode = U32(h + 48);
        const int32_t rate = static_cast<int32_t>(U32(h + 52));
        s.rate = rate > 0 ? static_cast<uint32_t>(rate) : 0;
        s.channels = U16(h + 62);
        if (s.channels == 0) s.channels = (s.mode & kModeStereo) ? 2 : 1;
        if (s.mode & kModeMpeg) s.codec = Codec::Mpeg;
        else if ((s.mode & kMode16Bits) && !(s.mode & (kMode8Bits | kModeOtherCodecs)) && !(b.flags & kFlagBigEndianPcm))
            s.codec = Codec::Pcm16;
        data = (data + 31) & ~static_cast<uint64_t>(31);
        s.offset = data;
        if (data + s.bytes > fileBytes) return Fail(why, "truncated bank");
        data += s.bytes;
        b.samples.push_back(std::move(s));
        at += size;
    }
    *out = std::move(b);
    return true;
}

int FindLine(const Bank& b, const std::string& name) {
    for (size_t i = 0; i < b.samples.size(); ++i)
        if (b.samples[i].name == name) return static_cast<int>(i);
    for (size_t i = 0; i < b.samples.size(); ++i) {
        const std::string& s = b.samples[i].name;
        if (s.size() != name.size()) continue;
        size_t k = 0;
        while (k < s.size() && Lower(s[k]) == Lower(name[k])) ++k;
        if (k == s.size()) return static_cast<int>(i);
    }
    return -1;
}

bool DecodeSample(const Sample& s, const uint8_t* data, size_t n, Pcm* out, std::string* why) {
    if (s.channels < 1 || s.channels > 2) return Fail(why, "unsupported sample format");
    if (s.frames == 0 || s.frames > kMaxFrames) return Fail(why, "undecodable sample");
    Pcm pcm;
    if (s.codec == Codec::Pcm16) {
        if (s.rate == 0) return Fail(why, "undecodable sample");
        const size_t frameBytes = 2u * s.channels;
        size_t frames = n / frameBytes;
        if (frames > s.frames) frames = s.frames;
        if (frames == 0) return Fail(why, "undecodable sample");
        pcm.channels = s.channels;
        pcm.rate = s.rate;
        pcm.frames = static_cast<uint32_t>(frames);
        pcm.samples.resize(frames * s.channels);
        for (size_t i = 0; i < pcm.samples.size(); ++i) pcm.samples[i] = static_cast<int16_t>(U16(data + 2 * i));
        *out = std::move(pcm);
        return true;
    }
    if (s.codec != Codec::Mpeg) return Fail(why, "unsupported sample format");

    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3d_sample_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t at = 0;
    pcm.samples.reserve(static_cast<size_t>(s.frames) * s.channels);
    while (at < n && pcm.frames < s.frames) {
        mp3dec_frame_info_t info = {};
        const size_t left = n - at;
        const int got = mp3dec_decode_frame(&dec, data + at, static_cast<int>(left > 0x7fffffff ? 0x7fffffff : left), frame, &info);
        if (info.frame_bytes <= 0) break;  // nothing more that looks like a frame
        at += static_cast<size_t>(info.frame_bytes);
        if (got <= 0) continue;  // a frame skipped (for layer III, one that needs data from before the sample)
        if (info.channels < 1 || info.channels > 2 || info.hz <= 0) return Fail(why, "undecodable sample");
        if (pcm.channels == 0) {
            pcm.channels = static_cast<uint16_t>(info.channels);
            pcm.rate = static_cast<uint32_t>(info.hz);
        } else if (pcm.channels != info.channels || pcm.rate != static_cast<uint32_t>(info.hz)) {
            return Fail(why, "undecodable sample");
        }
        uint32_t take = static_cast<uint32_t>(got);
        if (take > s.frames - pcm.frames) take = s.frames - pcm.frames;
        pcm.samples.insert(pcm.samples.end(), frame, frame + static_cast<size_t>(take) * pcm.channels);
        pcm.frames += take;
    }
    if (pcm.frames == 0) return Fail(why, "undecodable sample");
    *out = std::move(pcm);
    return true;
}
}  // namespace melange::audio::fsb
