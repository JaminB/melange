#pragma once
// The frame section shared by holo-media.exe (writer) and melange.asi (reader): a fixed header and three BGR0 slots,
// a lock-free triple buffer. Compiled into the x86 core and the x64 helper, so the layout is a packed struct with
// static_asserted offsets, 32-bit atomics only and a double for the pts. Frozen for Melange 0.12.0.
//
// Protocol. `latest` = slot | seq << 2 (slot 3 = no frame yet); `reading` = the slot the core is copying, or 3.
//   Writer: Begin() picks the slot that is neither latest's nor reading's, renders into it, Publish() writes the
//           slot's meta and then stores latest with release ordering.
//   Reader: Acquire() loads latest, announces the slot in `reading`, and re-loads latest; if it moved, it retries
//           (a writer that read `reading` before the announcement must have moved latest away from that slot before
//           choosing it, so the re-check catches every unsafe interleaving). The meta is then copied out and checked
//           against the reader's OWN limits (Critique S4): the header can be written by a hostile or broken helper,
//           so maxW/maxH/stride in the section are never trusted. Release() clears `reading`.
// The section is created by the core with its own size; the reader only ever touches [0, SectionBytes()).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "media/media_proto.h"

namespace melange::media::shm {
inline constexpr uint32_t kMagic = 0x31464D48u;  // "HMF1" as little-endian bytes
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kSlots = 3;
inline constexpr uint32_t kNone = 3;             // the "no slot" value of latest's low bits and of reading
inline constexpr uint32_t kFmtBgr0 = 1;          // 4 bytes per pixel, B G R X (mpv "bgr0"), GL_BGRA upload
inline constexpr uint32_t kMaxW = proto::kFrameMaxW;
inline constexpr uint32_t kMaxH = proto::kFrameMaxH;
inline constexpr size_t kDataOffset = 4096;      // slot 0 starts one page in (aligned copies)
inline constexpr size_t kSlotBytes = static_cast<size_t>(kMaxW) * kMaxH * 4;

#pragma pack(push, 1)
struct SlotMeta {
    uint32_t w, h, stride, fmt;
    uint32_t frameNo;   // the helper's own frame counter, for stats
    uint32_t reserved;
    double pts;         // mpv's video pts in seconds, or -1
};
struct Header {
    uint32_t magic, version, maxW, maxH;  // written by the core at creation; informational for the helper
    uint32_t latest;                      // atomic: slot | seq << 2
    uint32_t reading;                     // atomic: slot or kNone
    uint32_t writerPid;                   // the helper's pid once it mapped the section (diagnostics only)
    uint32_t reserved0;
    SlotMeta slots[kSlots];  // per-slot meta: a frame's size travels with its pixels, never torn from them
};
#pragma pack(pop)
static_assert(sizeof(SlotMeta) == 32);
static_assert(offsetof(SlotMeta, pts) == 24);
static_assert(sizeof(Header) == 128);
static_assert(offsetof(Header, latest) == 16 && offsetof(Header, reading) == 20 && offsetof(Header, slots) == 32);
static_assert(kDataOffset >= sizeof(Header) && kDataOffset % 64 == 0);

constexpr size_t SectionBytes() { return kDataOffset + kSlots * kSlotBytes; }  // 11,063,296
constexpr uint32_t SlotOf(uint32_t latest) { return latest & 3u; }
constexpr uint32_t SeqOf(uint32_t latest) { return latest >> 2; }
inline uint8_t* SlotData(void* base, uint32_t slot) { return static_cast<uint8_t*>(base) + kDataOffset + slot * kSlotBytes; }
inline const uint8_t* SlotData(const void* base, uint32_t slot) {
    return static_cast<const uint8_t*>(base) + kDataOffset + slot * kSlotBytes;
}

// The header's two control words are naturally aligned (offsets 16 and 20 of a page-aligned mapping).
inline std::atomic_ref<uint32_t> Word(Header* h, uint32_t Header::*field) { return std::atomic_ref<uint32_t>(h->*field); }

// The frame checks the reader applies with its own limits. A frame that fails is skipped and counted.
inline bool ValidMeta(const SlotMeta& m, uint32_t maxW = kMaxW, uint32_t maxH = kMaxH) {
    return m.fmt == kFmtBgr0 && m.w >= 1 && m.w <= maxW && m.h >= 1 && m.h <= maxH && m.stride == m.w * 4u &&
           static_cast<size_t>(m.stride) * m.h <= kSlotBytes;
}

// Core side, once, on a fresh zeroed mapping of SectionBytes().
inline void InitHeader(void* base) {
    Header* h = static_cast<Header*>(base);
    std::memset(h, 0, sizeof(Header));
    h->magic = kMagic;
    h->version = kVersion;
    h->maxW = kMaxW;
    h->maxH = kMaxH;
    Word(h, &Header::latest).store(kNone, std::memory_order_seq_cst);
    Word(h, &Header::reading).store(kNone, std::memory_order_seq_cst);
}

// ---------------------------------------------------------------- writer (helper)
class Writer {
public:
    explicit Writer(void* base) : h_(static_cast<Header*>(base)), base_(base) {}
    bool HeaderOk() const { return h_->magic == kMagic && h_->version == kVersion; }
    // The slot to render into next: never the published one, never the one being read.
    uint32_t Begin() {
        const uint32_t l = SlotOf(Word(h_, &Header::latest).load(std::memory_order_seq_cst));
        const uint32_t r = Word(h_, &Header::reading).load(std::memory_order_seq_cst);
        for (uint32_t s = 0; s < kSlots; ++s)
            if (s != l && s != r) return s;
        return 0;  // unreachable with three slots
    }
    uint8_t* Data(uint32_t slot) { return SlotData(base_, slot); }
    void Publish(uint32_t slot, const SlotMeta& meta) {
        std::memcpy(&h_->slots[slot], &meta, sizeof meta);
        seq_ = (seq_ + 1) & 0x3FFFFFFFu;
        Word(h_, &Header::latest).store(slot | (seq_ << 2), std::memory_order_seq_cst);
    }

private:
    Header* h_;
    void* base_;
    uint32_t seq_ = 0;
};

// ---------------------------------------------------------------- reader (core)
enum class ReadResult : uint8_t { None, Same, New, Bad };
struct Frame {
    uint32_t slot = kNone, seq = 0;
    SlotMeta meta{};
    const uint8_t* data = nullptr;  // meta.stride * meta.h valid bytes, within the section
};

class Reader {
public:
    // maxW/maxH are the reader's own limits (the creation values), never the header's.
    explicit Reader(void* base, uint32_t maxW = kMaxW, uint32_t maxH = kMaxH) : h_(static_cast<Header*>(base)), base_(base), maxW_(maxW), maxH_(maxH) {}

    // New: `out` holds a checked frame and the slot is held until Release(). Same: no newer frame than the last New.
    // None: nothing published yet, or the writer kept moving (try next frame). Bad: a frame failed the checks
    // (counted; nothing held).
    ReadResult Acquire(Frame* out) {
        if (h_->magic != kMagic || h_->version != kVersion) return Bad();
        for (int tries = 0; tries < 4; ++tries) {
            const uint32_t l = Word(h_, &Header::latest).load(std::memory_order_seq_cst);
            const uint32_t s = SlotOf(l);
            if (s >= kSlots) return ReadResult::None;
            if (have_ && SeqOf(l) == lastSeq_) return ReadResult::Same;
            Word(h_, &Header::reading).store(s, std::memory_order_seq_cst);
            if (Word(h_, &Header::latest).load(std::memory_order_seq_cst) != l) {
                Word(h_, &Header::reading).store(kNone, std::memory_order_seq_cst);
                continue;
            }
            SlotMeta m;
            std::memcpy(&m, &h_->slots[s], sizeof m);  // one copy: the checks and the caller see the same values
            if (!ValidMeta(m, maxW_, maxH_)) {
                Word(h_, &Header::reading).store(kNone, std::memory_order_seq_cst);
                lastSeq_ = SeqOf(l);
                have_ = true;
                return Bad();
            }
            out->slot = s;
            out->seq = SeqOf(l);
            out->meta = m;
            out->data = SlotData(static_cast<const void*>(base_), s);
            lastSeq_ = out->seq;
            have_ = true;
            return ReadResult::New;
        }
        return ReadResult::None;
    }
    void Release() { Word(h_, &Header::reading).store(kNone, std::memory_order_seq_cst); }
    void Reset() { have_ = false; }  // after a reopen: the next published frame counts as new
    uint64_t BadFrames() const { return bad_; }

private:
    ReadResult Bad() {
        ++bad_;
        return ReadResult::Bad;
    }
    Header* h_;
    void* base_;
    uint32_t maxW_, maxH_;
    uint32_t lastSeq_ = 0;
    bool have_ = false;
    uint64_t bad_ = 0;
};
}  // namespace melange::media::shm
