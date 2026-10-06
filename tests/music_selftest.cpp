// Offline self-test of the music module's data side: spice.json "music" parsing, the MP3 frame scanner on synthetic
// MPEG frames (ID3v2 tag, junk between frames, an ID3v1 tail), the FSB4 bank bytes, the first-track shuffle rule and
// the CreateFileA path matcher.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mods/spice.h"
#include "music/bank.h"

namespace fs = std::filesystem;
namespace mu = melange::music;

namespace {
int g_checks = 0, g_failed = 0;

void Check(bool ok, const char* what, const std::string& detail = {}) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
}

using Bytes = std::vector<uint8_t>;
void Append(Bytes& b, const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }

// One valid frame. mpeg1: 1 = MPEG-1 else MPEG-2; layer 3 or 2; bitrateIx 1..14; rateIx 0..2; mono sets channel mode 3.
// The body is filled with a marker so the bank's bytes can be traced back to their frame.
Bytes Frame(bool mpeg1, int layer, int bitrateIx, int rateIx, bool pad, bool mono, uint8_t fill, size_t* lengthOut = nullptr) {
    static const int br1l3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int br1l2[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
    static const int brLsf[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const int r1[] = {44100, 48000, 32000}, r2[] = {22050, 24000, 16000};
    const int kbps = !mpeg1 ? brLsf[bitrateIx] : layer == 3 ? br1l3[bitrateIx] : br1l2[bitrateIx];
    const int rate = mpeg1 ? r1[rateIx] : r2[rateIx];
    const int len = (!mpeg1 && layer == 3 ? 72 : 144) * kbps * 1000 / rate + (pad ? 1 : 0);
    Bytes f(static_cast<size_t>(len), fill);
    f[0] = 0xFF;
    f[1] = static_cast<uint8_t>(0xE0 | ((mpeg1 ? 3 : 2) << 3) | ((layer == 3 ? 1 : 2) << 1) | 1);
    f[2] = static_cast<uint8_t>((bitrateIx << 4) | (rateIx << 2) | (pad ? 2 : 0));
    f[3] = static_cast<uint8_t>(mono ? 0xC4 : 0x44);   // channel mode 3 = mono, 1 = joint stereo; fill bits otherwise
    if (lengthOut) *lengthOut = f.size();
    return f;
}

// A file: ID3v2 tag, then `n` frames with `junk` bytes between every pair, then an ID3v1 tail.
Bytes File(int n, bool mpeg1, int layer, int rateIx, bool mono, uint8_t fill, size_t junk = 7) {
    Bytes b = {'I', 'D', '3', 3, 0, 0, 0, 0, 0x02, 0x01};   // syncsafe size 257
    b.resize(b.size() + 257, 0x55);
    for (int i = 0; i < n; ++i) {
        Append(b, Frame(mpeg1, layer, 9, rateIx, i % 3 == 0, mono, fill));
        for (size_t j = 0; j < junk; ++j) b.push_back(static_cast<uint8_t>(j == 0 ? 0x00 : 0x41 + j));
    }
    Bytes tag(128, 0x20);
    memcpy(tag.data(), "TAG", 3);
    Append(b, tag);
    return b;
}

uint32_t U32(const Bytes& b, size_t at) { uint32_t v; memcpy(&v, &b[at], 4); return v; }
uint16_t U16(const Bytes& b, size_t at) { uint16_t v; memcpy(&v, &b[at], 2); return v; }
float F32(const Bytes& b, size_t at) { float v; memcpy(&v, &b[at], 4); return v; }

mu::Track Scan(const Bytes& f, bool* ok = nullptr, std::string* err = nullptr) {
    mu::Scan s = mu::ScanMp3(f.data(), f.size());
    if (ok) *ok = s.ok;
    if (err) *err = s.error;
    return s.track;
}

void TestScanner() {
    {   // MPEG-1 Layer III 48 kHz stereo with the junk and tags around the frames
        const Bytes f = File(80, true, 3, 1, false, 0x11);
        bool ok = false;
        mu::Track t = Scan(f, &ok);
        Check(ok, "MPEG-1 L3 accepted");
        Check(t.frameCount == 80, "MPEG-1 L3 frame count", std::to_string(t.frameCount));
        Check(t.samples == 80u * 1152, "MPEG-1 L3 sample count", std::to_string(t.samples));
        Check(t.sampleRate == 48000 && t.channels == 2, "MPEG-1 L3 rate and channels");
        size_t expect = 0;
        for (int i = 0; i < 80; ++i) {
            size_t len = 0;
            Frame(true, 3, 9, 1, i % 3 == 0, false, 0, &len);
            expect += len;
        }
        Check(t.frames.size() == expect, "frame bytes are the frames only", std::to_string(t.frames.size()) + " vs " + std::to_string(expect));
        Check(!t.frames.empty() && t.frames[0] == 0xFF, "frames start at a sync");
    }
    {   // MPEG-2 Layer III 24 kHz mono: 576 samples a frame
        bool ok = false;
        mu::Track t = Scan(File(60, false, 3, 1, true, 0x22), &ok);
        Check(ok && t.frameCount == 60, "MPEG-2 L3 accepted");
        Check(t.samples == 60u * 576, "MPEG-2 L3 sample count", std::to_string(t.samples));
        Check(t.sampleRate == 24000 && t.channels == 1, "MPEG-2 L3 rate and mono");
    }
    {   // Layer II, MPEG-1 44.1 kHz: 1152 samples a frame
        bool ok = false;
        mu::Track t = Scan(File(55, true, 2, 0, false, 0x33), &ok);
        Check(ok && t.frameCount == 55 && t.samples == 55u * 1152 && t.sampleRate == 44100, "MPEG-1 L2 accepted");
    }
    {   // no tag, no junk, nothing after the last frame
        Bytes f;
        for (int i = 0; i < 50; ++i) Append(f, Frame(true, 3, 9, 1, false, false, 0x44));
        bool ok = false;
        mu::Track t = Scan(f, &ok);
        Check(ok && t.frameCount == 50 && t.frames.size() == f.size(), "exactly the minimum, no tags");
    }
    {   // a frame cut off at the end of the file is not counted
        Bytes f;
        for (int i = 0; i < 52; ++i) Append(f, Frame(true, 3, 9, 1, false, false, 0x44));
        f.resize(f.size() - 10);
        bool ok = false;
        mu::Track t = Scan(f, &ok);
        Check(ok && t.frameCount == 51, "a truncated last frame is dropped", std::to_string(t.frameCount));
    }
    {   // too few frames
        bool ok = true;
        std::string err;
        Scan(File(49, true, 3, 1, false, 0x11), &ok, &err);
        Check(!ok && !err.empty(), "49 frames refused", err);
    }
    {   // a rate change inside the file
        Bytes f = File(40, true, 3, 1, false, 0x11);
        Append(f, File(40, true, 3, 0, false, 0x11));
        bool ok = true;
        std::string err;
        Scan(f, &ok, &err);
        Check(!ok && err.find("48000") != std::string::npos && err.find("44100") != std::string::npos, "rate change refused", err);
    }
    {   // a channel change inside the file
        Bytes f = File(40, true, 3, 1, false, 0x11);
        Append(f, File(40, true, 3, 1, true, 0x11));
        bool ok = true;
        Scan(f, &ok);
        Check(!ok, "channel change refused");
    }
    {   // not MPEG at all
        Bytes f(5000, 0x41);
        bool ok = true;
        Scan(f, &ok);
        Check(!ok, "garbage refused");
        Check(!mu::ScanMp3(nullptr, 0).ok, "empty refused");
    }
    {   // invalid headers are junk: free-format bitrate, reserved sample rate, Layer I, MPEG 2.5
        Bytes f;
        const uint8_t bad[][4] = {{0xFF, 0xFB, 0x00, 0x44}, {0xFF, 0xFB, 0x9C, 0x44}, {0xFF, 0xFF, 0x90, 0x44}, {0xFF, 0xE3, 0x90, 0x44},
                                  {0xFF, 0xFB, 0xF0, 0x44}};
        for (const auto& h : bad) f.insert(f.end(), h, h + 4);
        for (int i = 0; i < 50; ++i) Append(f, Frame(true, 3, 9, 1, false, false, 0x44));
        bool ok = false;
        mu::Track t = Scan(f, &ok);
        Check(ok && t.frameCount == 50, "invalid headers are skipped as junk", std::to_string(t.frameCount));
    }
}

void TestBank() {
    Bytes vanilla(mu::kHeaderBytes);
    for (size_t i = 0; i < vanilla.size(); ++i) vanilla[i] = static_cast<uint8_t>(i * 3 + 1);
    memcpy(vanilla.data(), "FSB4", 4);
    const uint32_t one = 1;
    memcpy(&vanilla[4], &one, 4);
    std::string err;
    Check(mu::CheckVanillaHeader(vanilla.data(), vanilla.size(), &err), "vanilla header accepted");
    Bytes bad = vanilla;
    bad[0] = 'X';
    Check(!mu::CheckVanillaHeader(bad.data(), bad.size(), &err), "vanilla header with another tag refused");
    bad = vanilla;
    bad[4] = 2;
    Check(!mu::CheckVanillaHeader(bad.data(), bad.size(), &err), "vanilla header with 2 samples refused");
    Check(!mu::CheckVanillaHeader(vanilla.data(), 20, &err), "short vanilla header refused");

    const mu::Track a = Scan(File(60, true, 3, 1, false, 0xA1));
    const mu::Track b = Scan(File(70, true, 3, 1, false, 0xB2));
    Bytes bank;
    Check(mu::BuildBank(vanilla.data(), {&b, &a}, &bank, &err), "bank built", err);
    const size_t lengthBytes = a.frames.size() + b.frames.size();
    const size_t dataSize = (lengthBytes + 15) / 16 * 16;
    Check(bank.size() == 48 + 80 + dataSize, "file length is 48 + 80 + datasize", std::to_string(bank.size()));
    Check(memcmp(bank.data(), "FSB4", 4) == 0, "FSB4 tag");
    Check(U32(bank, 4) == 1, "numsamples 1");
    Check(U32(bank, 8) == 80, "shdrsize 80");
    Check(U32(bank, 12) == dataSize, "datasize rounded up to 16", std::to_string(U32(bank, 12)));
    Check(memcmp(&bank[16], &vanilla[16], 32) == 0, "32 vanilla bytes copied verbatim");
    Check(U16(bank, 48) == 80, "sample header size");
    Check(memcmp(&bank[50], "SuddenDeath", 11) == 0 && bank[61] == 0 && bank[79] == 0, "name, zero padded to 30");
    Check(U32(bank, 80) == (a.samples + b.samples), "lengthsamples", std::to_string(U32(bank, 80)));
    Check(U32(bank, 84) == lengthBytes, "lengthbytes");
    Check(U32(bank, 88) == 0, "loopstart 0");
    Check(U32(bank, 92) == a.samples + b.samples - 1, "loopend lengthsamples-1");
    Check(U32(bank, 96) == 0x00040200, "mode 0x00040200");
    Check(U32(bank, 100) == 48000, "deffreq");
    Check(U16(bank, 104) == 255 && U16(bank, 106) == 128 && U16(bank, 108) == 128, "defvol, defpan, defpri");
    Check(U16(bank, 110) == 2, "numchannels");
    Check(F32(bank, 112) == 1.0f && F32(bank, 116) == 10000.0f, "min and max distance");
    Check(U32(bank, 120) == 80, "varfreq 80");
    Check(U16(bank, 124) == 0 && U16(bank, 126) == 0, "varvol, varpan 0");
    Check(memcmp(&bank[128], b.frames.data(), b.frames.size()) == 0, "first track's frames first (hard cut)");
    Check(memcmp(&bank[128 + b.frames.size()], a.frames.data(), a.frames.size()) == 0, "second track's frames follow");
    bool zeros = true;
    for (size_t i = 128 + lengthBytes; i < bank.size(); ++i) zeros &= bank[i] == 0;
    Check(zeros, "datasize padding is zero");

    // mono, and a single track whose byte count is already a multiple of 16
    const mu::Track m = Scan(File(60, false, 3, 1, true, 0xC3));
    Check(mu::BuildBank(vanilla.data(), {&m}, &bank, &err) && U16(bank, 110) == 1 && U32(bank, 100) == 24000, "mono, 24 kHz bank");
    mu::Track aligned = a;
    aligned.frames.resize(1024);
    Check(mu::BuildBank(vanilla.data(), {&aligned}, &bank, &err) && U32(bank, 12) == 1024 && bank.size() == 48 + 80 + 1024, "no padding when aligned");

    // tracks that differ in rate or channels are refused
    const mu::Track other = Scan(File(60, true, 3, 0, false, 0xD4));
    Check(!mu::BuildBank(vanilla.data(), {&a, &other}, &bank, &err), "rate mismatch refused");
    Check(!mu::BuildBank(vanilla.data(), {&a, &m}, &bank, &err), "channel mismatch refused");
    Check(!mu::BuildBank(vanilla.data(), {}, &bank, &err), "no tracks refused");
}

void TestShuffle() {
    std::mt19937 rng(12345);
    bool differs = true, perm = true, single = true;
    std::map<size_t, int> firsts;
    for (int i = 0; i < 2000; ++i) {
        const size_t n = 2 + static_cast<size_t>(i % 5), last = static_cast<size_t>(i) % n;
        const auto o = mu::ShuffleOrder(n, last, rng);
        differs &= o.size() == n && o[0] != last;
        std::vector<size_t> s = o;
        std::sort(s.begin(), s.end());
        for (size_t k = 0; k < n; ++k) perm &= s[k] == k;
    }
    Check(differs, "first track differs from the last first track over 2000 draws");
    Check(perm, "every order is a permutation");
    for (int i = 0; i < 3000; ++i) ++firsts[mu::ShuffleOrder(3, 1, rng)[0]];
    Check(firsts[1] == 0 && firsts[0] > 1200 && firsts[2] > 1200, "the other tracks lead about equally", std::to_string(firsts[0]) + "/" + std::to_string(firsts[2]));
    // no constraint: unknown or absent last, and every track leads sometimes
    firsts.clear();
    for (int i = 0; i < 3000; ++i) ++firsts[mu::ShuffleOrder(3, mu::kNoTrack, rng)[0]];
    Check(firsts[0] > 800 && firsts[1] > 800 && firsts[2] > 800, "unconstrained shuffle is uniform");
    const auto one = mu::ShuffleOrder(1, 0, rng);
    single = one.size() == 1 && one[0] == 0;
    Check(single, "a single track is allowed to repeat");
    Check(mu::ShuffleOrder(0, mu::kNoTrack, rng).empty(), "no tracks, no order");
    Check(mu::ShuffleOrder(2, 99, rng).size() == 2, "an out-of-range last is no constraint");
}

void TestPath() {
    Check(mu::IsSuddenDeathPath("data\\audio\\PC\\muSuddenDeath.fsb"), "the game's relative path");
    Check(mu::IsSuddenDeathPath("C:\\Games\\Worms\\Data\\Audio\\PC\\muSuddenDeath.fsb"), "absolute path");
    Check(mu::IsSuddenDeathPath("DATA\\AUDIO\\PC\\MUSUDDENDEATH.FSB"), "upper case");
    Check(mu::IsSuddenDeathPath("data/audio/pc/musuddendeath.fsb"), "forward slashes");
    Check(mu::IsSuddenDeathPath("C:/Games/Worms/data\\audio/PC\\muSuddenDeath.fsb"), "mixed slashes and case");
    Check(!mu::IsSuddenDeathPath("data\\audio\\PC\\muMenu.fsb"), "another bank");
    Check(!mu::IsSuddenDeathPath("data\\audio\\PC\\muSuddenDeath.fsb.bak"), "a longer name");
    Check(!mu::IsSuddenDeathPath("data\\audio\\PC\\xmuSuddenDeath.fsb"), "a name ending in the string without a separator");
    Check(!mu::IsSuddenDeathPath("data\\audio\\PC\\muSuddenDeath.fsb\\more"), "a name that merely contains the string");
    Check(!mu::IsSuddenDeathPath("xaudio\\pc\\musuddendeath.fsb"), "a folder that ends in audio");
    Check(!mu::IsSuddenDeathPath("muSuddenDeath.fsb"), "the bare file name");
    Check(!mu::IsSuddenDeathPath(""), "empty");
    Check(!mu::IsSuddenDeathPath(nullptr), "null");
}

// ---------------------------------------------------------------- spice.json "music"
fs::path WriteMod(const std::string& id, const std::string& music) {
    const fs::path dir = fs::temp_directory_path() / "melange_music_selftest" / id;
    fs::create_directories(dir);
    std::ofstream(dir / "spice.json", std::ios::binary | std::ios::trunc)
        << "{\"spiceVersion\":1,\"id\":\"" << id << "\",\"version\":\"1.0.0\",\"kind\":\"client-only\",\"name\":\"T\",\"melange\":{\"range\":\">=0.5.0\"}"
        << (music.empty() ? "" : ",\"music\":" + music) << "}";
    return dir;
}

bool ParseMusic(const std::string& id, const std::string& music, melange::spice::Manifest* m, std::string* err) {
    std::vector<melange::spice::Error> errs;
    const bool ok = melange::spice::Parse(WriteMod(id, music).wstring(), m, &errs);
    if (err) *err = errs.empty() ? "" : errs[0].field + ": " + errs[0].text;
    return ok;
}

void TestManifest() {
    melange::spice::Manifest m;
    std::string err;
    Check(ParseMusic("mu-ok", "[{\"slot\":\"suddenDeath\",\"file\":\"music/ash-ridge.mp3\",\"title\":\"Ash Ridge\",\"credit\":\"Slaughter at Ash Ridge\"},"
                              "{\"slot\":\"suddenDeath\",\"file\":\"Music/B.MP3\",\"title\":\"B\"}]", &m, &err),
          "valid music parses", err);
    Check(m.music.size() == 2 && m.music[0].slot == "suddenDeath" && m.music[0].file == "music/ash-ridge.mp3" &&
              m.music[0].title == "Ash Ridge" && m.music[0].credit == "Slaughter at Ash Ridge" && m.music[1].credit.empty(),
          "music fields");
    Check(ParseMusic("mu-none", "", &m, &err) && m.music.empty(), "no music key");
    struct Bad { const char* what; std::string json; } bad[] = {
        {"unknown slot", "[{\"slot\":\"menu\",\"file\":\"a.mp3\",\"title\":\"A\"}]"},
        {"not an array", "{}"},
        {"not mp3", "[{\"slot\":\"suddenDeath\",\"file\":\"a.ogg\",\"title\":\"A\"}]"},
        {"path escape", "[{\"slot\":\"suddenDeath\",\"file\":\"../a.mp3\",\"title\":\"A\"}]"},
        {"absolute path", "[{\"slot\":\"suddenDeath\",\"file\":\"C:/a.mp3\",\"title\":\"A\"}]"},
        {"no title", "[{\"slot\":\"suddenDeath\",\"file\":\"a.mp3\"}]"},
        {"empty title", "[{\"slot\":\"suddenDeath\",\"file\":\"a.mp3\",\"title\":\"\"}]"},
        {"long title", "[{\"slot\":\"suddenDeath\",\"file\":\"a.mp3\",\"title\":\"" + std::string(49, 'x') + "\"}]"},
        {"non-ASCII title", "[{\"slot\":\"suddenDeath\",\"file\":\"a.mp3\",\"title\":\"caf\\u00e9\"}]"},
        {"long credit", "[{\"slot\":\"suddenDeath\",\"file\":\"a.mp3\",\"title\":\"A\",\"credit\":\"" + std::string(97, 'x') + "\"}]"},
        {"unknown key", "[{\"slot\":\"suddenDeath\",\"file\":\"a.mp3\",\"title\":\"A\",\"loop\":true}]"},
        {"no slot", "[{\"file\":\"a.mp3\",\"title\":\"A\"}]"},
    };
    int n = 0;
    for (const auto& b : bad) Check(!ParseMusic("mu-bad" + std::to_string(n++), b.json, &m, &err), (std::string("refused: ") + b.what).c_str());
    std::string many = "[";
    for (int i = 0; i < 17; ++i) many += std::string(i ? "," : "") + "{\"slot\":\"suddenDeath\",\"file\":\"a" + std::to_string(i) + ".mp3\",\"title\":\"A\"}";
    many += "]";
    Check(!ParseMusic("mu-many", many, &m, &err), "17 entries refused");
    many = many.substr(0, many.rfind(",{")) + "]";
    Check(ParseMusic("mu-16", many, &m, &err) && m.music.size() == 16, "16 entries accepted", err);
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "melange_music_selftest", ec);
}
}  // namespace

int main() {
    TestScanner();
    TestBank();
    TestShuffle();
    TestPath();
    TestManifest();
    printf("music_selftest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
