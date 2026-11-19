#pragma once
// FMOD FSB4 sound banks, the game's own speech (Data\Audio\PC\vo*.fsb): the sample table and one sample decoded to
// 16-bit PCM, for wum.audio.voice (Melange 0.17). Pure: no Windows, no engine, so the self-test covers it. The banks are
// read from the player's install at run time; Melange never ships them.
//
// Layout (little-endian): a 48-byte header "FSB4", u32 samples @4, u32 sample-header bytes @8, u32 data bytes @12,
// u32 version @16, u32 flags @20. Sample headers follow at 48, each u16 size, char name[30], u32 length (frames) @32,
// u32 compressed bytes @36, loop start/end @40/@44, u32 mode @48, i32 default rate @52, u16 volume, i16 pan,
// u16 priority, u16 channels @56..@62. Sample data starts at 48 + sample-header bytes; every sample starts on a 32-byte
// boundary of the file and is `compressed bytes` long.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/bank_name.h"

namespace melange::audio::fsb {
constexpr size_t kHeaderBytes = 48;
constexpr uint32_t kMaxSamples = 4096;             // a speech bank has about a hundred
constexpr uint32_t kMaxSampleHeaderBytes = 1u << 20;
constexpr uint32_t kMaxFrames = 48000u * 120u;     // two minutes at 48 kHz: a speech line is a few seconds

enum class Codec : uint8_t { Pcm16, Mpeg, Other };

struct Sample {
    std::string name;       // verbatim, NULs trimmed
    uint32_t frames = 0;    // samples per channel (the header's length)
    uint32_t rate = 0;      // the header's default frequency
    uint16_t channels = 0;  // 1 or 2 (from the header, else the mode's stereo flag)
    uint32_t mode = 0;
    Codec codec = Codec::Other;
    uint64_t offset = 0;    // of the data, from the start of the file
    uint32_t bytes = 0;     // of the data
    double Seconds() const { return rate ? static_cast<double>(frames) / rate : 0.0; }
};

struct Bank {
    uint32_t flags = 0;
    std::vector<Sample> samples;
};

// How many bytes from the start of the file ParseBank needs (the header and every sample header), from the first 48.
bool HeaderBytes(const uint8_t* p, size_t n, size_t* need, std::string* why);
// Parses the header and sample table. `p` holds at least HeaderBytes() bytes of the file, `fileBytes` is the file's
// size; every sample's data must lie inside it. False with *why ("not an FSB4 bank", "truncated bank", ...).
bool ParseBank(const uint8_t* p, size_t n, uint64_t fileBytes, Bank* out, std::string* why);

// The index of the line called `name`: an exact match first, then ASCII case-insensitive. -1 when there is none.
int FindLine(const Bank& b, const std::string& name);

struct Pcm {
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint32_t frames = 0;
    std::vector<int16_t> samples;  // interleaved
};
// Decodes one sample from its data bytes (`n` = s.bytes): raw 16-bit PCM is copied, MPEG (layers I-III, through
// minimp3) is decoded frame by frame and cut to the header's length. False with *why ("unsupported sample format",
// "undecodable sample", ...).
bool DecodeSample(const Sample& s, const uint8_t* data, size_t n, Pcm* out, std::string* why);

}  // namespace melange::audio::fsb
