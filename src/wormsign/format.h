#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The .wsr recording container, version 1 (little-endian):
//   "WSR1", then chunks {u32 type, u32 flags, u32 rawLen, u32 storedLen, u32 crc32, stored bytes}
//   flags bit 0: the stored bytes are raw deflate of the rawLen payload bytes; crc32 covers the stored bytes
//   it ends with an INDX chunk ({u32 type, u64 offset, u32 tickFrom, u32 tickTo} per earlier chunk, stored) and
//   the trailer {u64 indexOffset, "WSRE"}.
// A file without the trailer is read up to its first bad chunk and reported incomplete.
namespace melange::wormsign::wsr {
constexpr uint32_t Tag(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
}
constexpr uint32_t kFormat = 1;
constexpr uint32_t kHEAD = Tag("HEAD"), kSEED = Tag("SEED"), kPDRW = Tag("PDRW"), kSETP = Tag("SETP"), kINPT = Tag("INPT"),
                   kRMTI = Tag("RMTI"), kDISP = Tag("DISP"), kTICK = Tag("TICK"), kDETL = Tag("DETL"), kDVRG = Tag("DVRG"),
                   kENGV = Tag("ENGV"), kNOTE = Tag("NOTE"), kINDX = Tag("INDX"), kCTRB = Tag("CTRB");
constexpr uint32_t kFlagDeflate = 1;
constexpr uint32_t kMaxChunkBytes = 64u << 20;
constexpr size_t kMagicBytes = 4, kChunkHeaderBytes = 20, kIndexEntryBytes = 20, kTrailerBytes = 12;
constexpr uint64_t kMaxFileBytes = 1ull << 30;

struct IndexEntry {
    uint32_t type;
    uint64_t offset;
    uint32_t tickFrom, tickTo;
};

// One encoded chunk (header and stored bytes). Deflates at level 3 when that makes it smaller.
std::vector<uint8_t> EncodeChunk(uint32_t type, const void* data, size_t n, bool deflate);

class Writer {
  public:
    Writer() = default;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();
    bool Open(const std::wstring& path);
    bool OpenMemory(std::vector<uint8_t>* sink);
    bool Chunk(uint32_t type, const void* data, size_t n, bool deflate = true, uint32_t tickFrom = 0, uint32_t tickTo = 0);
    bool Flush();
    bool Close();                                  // writes INDX and the trailer
    void Abandon();                                // closes without INDX, as a crash would leave it
    bool IsOpen() const { return f_ || sink_; }
    uint64_t Bytes() const { return pos_; }

  private:
    bool Put(const void* p, size_t n);
    FILE* f_ = nullptr;
    std::vector<uint8_t>* sink_ = nullptr;
    uint64_t pos_ = 0;
    bool failed_ = false;
    std::vector<IndexEntry> index_;
};

struct ChunkRef {
    uint32_t type, flags, rawLen, storedLen;
    uint64_t offset;                               // of the chunk header
};

class Reader {
  public:
    bool OpenFile(const std::wstring& path, std::string* err);
    bool OpenMemory(std::vector<uint8_t> bytes, std::string* err);
    bool Complete() const { return complete_; }    // INDX and trailer present and consistent
    int Format() const { return format_; }
    int EngineHash() const { return engineHash_; }
    const std::string& Header() const { return head_; }  // the HEAD chunk's JSON
    const std::vector<ChunkRef>& Chunks() const { return chunks_; }  // every valid chunk, INDX included
    const std::vector<IndexEntry>& Index() const { return index_; }  // empty when incomplete
    bool Payload(const ChunkRef& c, std::vector<uint8_t>* out) const;  // inflates when needed
    // Calls fn(ref, payload) for every chunk of `type` (0: all but INDX). False if a payload failed to decode.
    template <class Fn>
    bool ForEach(uint32_t type, Fn&& fn) const {
        std::vector<uint8_t> p;
        for (const ChunkRef& c : chunks_) {
            if (type ? c.type != type : c.type == kINDX) continue;
            if (!Payload(c, &p)) return false;
            fn(c, p);
        }
        return true;
    }

  private:
    bool Parse(std::string* err);
    std::vector<uint8_t> data_;
    std::vector<ChunkRef> chunks_;
    std::vector<IndexEntry> index_;
    std::string head_;
    int format_ = 0, engineHash_ = 0;
    bool complete_ = false;
};
}  // namespace melange::wormsign::wsr
