#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "melange/wormsign.h"

// Field-level engine state per tick: the bytes the engine hash reads, kept for diffs. Filled by ComputeEngine in the
// same pass as the hash, then packed into a ring of the last 512 ticks.
namespace melange::wormsign::detail {
constexpr size_t kWormBytes = 93;            // the worm container bytes of the hash, in hash order
constexpr uint8_t kSlotUnreadable = 0x80;    // WormDetail.slot flag: the container could not be read
constexpr uint8_t kNoTeam = 0xff;            // TeamDetail.slot of an unused entry
constexpr uint32_t kRingTicks = 512;

struct WormDetail {
    uint8_t slot;
    uint8_t bytes[kWormBytes];
};
struct TeamDetail {
    uint8_t slot, active;
    uint32_t score;
};
struct ProjDetail {
    uint32_t vt, time;
    uint8_t cat;
    uint32_t pv[6];                          // position and velocity, raw float bits
};
// The active logical camera, as GameStateValidationMsg (0x68a027/0x68a335) checks it in reasons 7-10: camera
// manager *(0x95c370), camera = (+0x2a0 vector)[m_uLogicalCamera +0x28c]; its view matrix (Camera::BuildView 0x51aaa0)
// is built from pos +0x04, target +0x10 and up +0x1c only.
constexpr uint8_t kCamPresent = 1;           // CameraDetail.flags: the camera was read
struct CameraDetail {
    uint8_t flags;
    uint32_t index, count;                   // m_uLogicalCamera and the camera vector's size (detail only)
    uint32_t view;                           // camera +0x2c, m_eView (detail only)
    uint32_t pos[3], target[3], up[3];       // raw float bits
};
// The bytes the melange.camera contributor hashes: flags, pos, target, up (what reasons 7-10 compare). The index,
// count and view mode are left out: the engine compares the camera by name (reason 6), not by index.
constexpr size_t kCameraHashBytes = 1 + 36;
size_t CameraHashBytes(const CameraDetail& c, uint8_t out[kCameraHashBytes]);
uint64_t CameraHash(const CameraDetail& c);  // the contributor's hash: FNV-1a-64 of CameraHashBytes

struct DetailRec {
    uint32_t tick;
    uint8_t wormCount;
    WormDetail worms[16];
    TeamDetail teams[4];
    uint16_t projCount;
    ProjDetail proj[64];
    uint32_t rng, rng2;
    int32_t curTeam, activeWorm;
    CameraDetail cam;
};

void Clear(DetailRec* r);                    // empty record: no worms, teams, projectiles or camera

// The ring. Main thread writes; Get and GetPacked are safe from any thread.
DetailRec* Scratch();                        // cleared record for the tick being hashed
void Commit(const DetailRec& r);
void Reset();                                // session begin
bool Get(uint32_t tick, DetailRec* out);
bool GetPacked(uint32_t tick, std::vector<uint8_t>* out);
size_t RingBytes();

// Packed layout (little-endian): u32 tick, rng, rng2, i32 curTeam, activeWorm; u8 n + n x {u8 slot, 93 bytes};
// u8 n + n x {u8 slot, u8 active, u32 score}; u16 n + n x {u32 vt, u32 time, u8 cat, 6 x u32};
// camera {u8 flags, u32 index, count, view, 9 x u32 pos/target/up}. Records stay in this process (the ring) or go
// to DETL chunks, whose readers (tools/wsr/wsr.py) take the worms prefix only, so the camera tail needs no version.
constexpr size_t kPackedCamera = 1 + 3 * 4 + 9 * 4;
constexpr size_t kMaxPacked = 20 + 1 + 16 * (1 + kWormBytes) + 1 + 4 * 6 + 2 + 64 * 33 + kPackedCamera;
size_t Pack(const DetailRec& r, uint8_t* out, size_t cap);   // 0 when cap is too small
bool Unpack(const uint8_t* p, size_t n, DetailRec* out);

// Delta against the previous packed record: u8 0 + u16 len + bytes (key), or u8 1 + u16 len + runs of
// {u16 same, u16 changed, changed bytes XOR prev}. The recorder's DETL chunks use this.
void EncodeDelta(const uint8_t* prev, size_t prevLen, const uint8_t* cur, size_t curLen, std::vector<uint8_t>* out);
// Consumes one encoded record from `p`; returns the bytes used, 0 when malformed.
size_t DecodeDelta(const uint8_t* prev, size_t prevLen, const uint8_t* p, size_t n, std::vector<uint8_t>* out);

// The engine components a record reproduces (all but the task queue): bit i of the result is set for c[i] filled.
uint8_t Recompute(const DetailRec& r, uint64_t c[kEngineComps]);

// Named fields (pos.x, energy, ...), for reports. Off the tick path: these format floats.
std::string ToJson(const DetailRec& r);
std::string Diff(const DetailRec& a, const DetailRec& b);   // one "path a -> b" line per differing field
}  // namespace melange::wormsign::detail
