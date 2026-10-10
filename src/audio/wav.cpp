#include "audio/wav.h"

#include <cstring>

namespace melange::audio {
namespace {
uint16_t Rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool Fail(std::string* why, const char* msg) {
    if (why) *why = msg;
    return false;
}
constexpr uint16_t kPcm = 1, kExtensible = 0xFFFE;

// KSDATAFORMAT_SUBTYPE_PCM is 00000001-0000-0010-8000-00AA00389B71: the format tag, then this tail.
bool IsPcmSubtype(const uint8_t* g) {
    static const uint8_t kGuidTail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    return Rd32(g) == kPcm && memcmp(g + 4, kGuidTail, 12) == 0;
}
}  // namespace

bool ParseWav(const uint8_t* p, size_t n, WavInfo* out, std::string* why) {
    if (!p || !out) return Fail(why, "no data");
    if (n > kMaxWavBytes) return Fail(why, "too large");
    if (n < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) return Fail(why, "not a RIFF/WAVE file");

    bool haveFmt = false;
    uint16_t channels = 0, bits = 0, align = 0;
    uint32_t rate = 0;
    // The RIFF size field is ignored: writers that stream get it wrong, and the file length bounds every chunk anyway.
    size_t pos = 12;
    while (pos + 8 <= n) {
        const uint8_t* id = p + pos;
        const size_t size = Rd32(p + pos + 4);
        const size_t body = pos + 8;
        if (size > n - body) return Fail(why, "truncated");
        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16) return Fail(why, "fmt chunk too short");
            const uint16_t tag = Rd16(p + body);
            channels = Rd16(p + body + 2);
            rate = Rd32(p + body + 4);
            align = Rd16(p + body + 12);
            bits = Rd16(p + body + 14);
            if (tag == kExtensible) {
                // WAVEFORMATEXTENSIBLE: cbSize(2) validBits(2) channelMask(4) subformat GUID(16)
                if (size < 40 || Rd16(p + body + 16) < 22) return Fail(why, "fmt chunk too short");
                if (!IsPcmSubtype(p + body + 24)) return Fail(why, "not PCM");
                if (Rd16(p + body + 18) != 16) return Fail(why, "not 16-bit");
            } else if (tag != kPcm) {
                return Fail(why, "not PCM");
            }
            haveFmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!haveFmt) return Fail(why, "data before fmt");
            if (bits != 16) return Fail(why, "not 16-bit");
            if (channels < 1 || channels > 2) return Fail(why, "needs 1 or 2 channels");
            if (rate < kMinRate || rate > kMaxRate) return Fail(why, "sample rate outside 8000-48000 Hz");
            if (align != channels * 2u) return Fail(why, "inconsistent block size");
            const uint32_t frames = static_cast<uint32_t>(size / align);  // a partial last frame is dropped
            if (frames == 0) return Fail(why, "no samples");
            out->channels = channels;
            out->rate = rate;
            out->frames = frames;
            out->data = p + body;
            out->bytes = static_cast<size_t>(frames) * align;
            return true;
        }
        pos = body + size + (size & 1);  // chunks are word-aligned; the pad byte is not counted in the size
    }
    return Fail(why, haveFmt ? "no data chunk" : "no fmt chunk");
}
}  // namespace melange::audio
