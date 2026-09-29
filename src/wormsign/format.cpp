#include "wormsign/format.h"

#include <miniz.h>

#include <cstring>

#include "tools/json_read.h"

namespace melange::wormsign::wsr {
namespace {
constexpr uint8_t kMagic[4] = {'W', 'S', 'R', '1'};
constexpr uint8_t kEnd[4] = {'W', 'S', 'R', 'E'};

void Le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void Le64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
uint32_t Rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
uint64_t Rd64(const uint8_t* p) { return Rd32(p) | static_cast<uint64_t>(Rd32(p + 4)) << 32; }
uint32_t Crc(const uint8_t* p, size_t n) { return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, p, n)); }
}  // namespace

std::vector<uint8_t> EncodeChunk(uint32_t type, const void* data, size_t n, bool deflate) {
    const auto* src = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> out;
    if (n > kMaxChunkBytes || (n && !src)) return out;
    void* packed = nullptr;
    size_t packedLen = 0;
    if (deflate && n >= 64) {
        const int flags = static_cast<int>(tdefl_create_comp_flags_from_zip_params(3, -15, MZ_DEFAULT_STRATEGY));
        packed = tdefl_compress_mem_to_heap(src, n, &packedLen, flags);
        if (packed && packedLen >= n) {
            mz_free(packed);
            packed = nullptr;
        }
    }
    const uint8_t* stored = packed ? static_cast<const uint8_t*>(packed) : src;
    const size_t storedLen = packed ? packedLen : n;
    out.reserve(kChunkHeaderBytes + storedLen);
    Le32(out, type);
    Le32(out, packed ? kFlagDeflate : 0);
    Le32(out, static_cast<uint32_t>(n));
    Le32(out, static_cast<uint32_t>(storedLen));
    Le32(out, storedLen ? Crc(stored, storedLen) : Crc(nullptr, 0));
    if (storedLen) out.insert(out.end(), stored, stored + storedLen);
    if (packed) mz_free(packed);
    return out;
}

Writer::~Writer() { Abandon(); }

bool Writer::Open(const std::wstring& path) {
    Abandon();
    f_ = _wfopen(path.c_str(), L"wb");
    if (!f_) return false;
    failed_ = false;
    pos_ = 0;
    index_.clear();
    return Put(kMagic, sizeof kMagic);
}

bool Writer::OpenMemory(std::vector<uint8_t>* sink) {
    Abandon();
    if (!sink) return false;
    sink_ = sink;
    sink_->clear();
    failed_ = false;
    pos_ = 0;
    index_.clear();
    return Put(kMagic, sizeof kMagic);
}

bool Writer::Put(const void* p, size_t n) {
    if (failed_) return false;
    if (sink_) {
        const auto* b = static_cast<const uint8_t*>(p);
        sink_->insert(sink_->end(), b, b + n);
    } else if (!f_ || fwrite(p, 1, n, f_) != n) {
        failed_ = true;
        return false;
    }
    pos_ += n;
    return true;
}

bool Writer::Chunk(uint32_t type, const void* data, size_t n, bool deflate, uint32_t tickFrom, uint32_t tickTo) {
    if (!IsOpen() || type == kINDX) return false;
    const std::vector<uint8_t> c = EncodeChunk(type, data, n, deflate);
    if (c.empty()) return false;
    const uint64_t at = pos_;
    if (!Put(c.data(), c.size())) return false;
    index_.push_back(IndexEntry{type, at, tickFrom, tickTo});
    return true;
}

bool Writer::Flush() { return !failed_ && (sink_ || (f_ && fflush(f_) == 0)); }

bool Writer::Close() {
    if (!IsOpen()) return false;
    std::vector<uint8_t> idx;
    idx.reserve(index_.size() * kIndexEntryBytes);
    for (const IndexEntry& e : index_) {
        Le32(idx, e.type);
        Le64(idx, e.offset);
        Le32(idx, e.tickFrom);
        Le32(idx, e.tickTo);
    }
    const uint64_t at = pos_;
    const std::vector<uint8_t> c = EncodeChunk(kINDX, idx.data(), idx.size(), false);
    std::vector<uint8_t> trailer;
    Le64(trailer, at);
    trailer.insert(trailer.end(), kEnd, kEnd + 4);
    bool ok = !c.empty() && Put(c.data(), c.size()) && Put(trailer.data(), trailer.size());
    if (f_) ok = fclose(f_) == 0 && ok;
    f_ = nullptr;
    sink_ = nullptr;
    return ok && !failed_;
}

void Writer::Abandon() {
    if (f_) fclose(f_);
    f_ = nullptr;
    sink_ = nullptr;
}

bool Reader::OpenFile(const std::wstring& path, std::string* err) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (err) *err = "cannot open the file";
        return false;
    }
    std::vector<uint8_t> d;
    std::vector<uint8_t> chunk(65536);
    size_t n;
    while ((n = fread(chunk.data(), 1, chunk.size(), f)) > 0) {
        if (d.size() + n > kMaxFileBytes) {
            fclose(f);
            if (err) *err = "file too large";
            return false;
        }
        d.insert(d.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
    }
    fclose(f);
    return OpenMemory(std::move(d), err);
}

bool Reader::OpenMemory(std::vector<uint8_t> bytes, std::string* err) {
    data_ = std::move(bytes);
    chunks_.clear();
    index_.clear();
    head_.clear();
    format_ = engineHash_ = 0;
    complete_ = false;
    std::string e;
    const bool ok = Parse(&e);
    if (!ok && err) *err = e;
    return ok;
}

bool Reader::Payload(const ChunkRef& c, std::vector<uint8_t>* out) const {
    const uint8_t* stored = data_.data() + c.offset + kChunkHeaderBytes;
    if (!(c.flags & kFlagDeflate)) {
        out->assign(stored, stored + c.storedLen);
        return true;
    }
    out->resize(c.rawLen);
    if (!c.rawLen || !c.storedLen) return false;
    const size_t got = tinfl_decompress_mem_to_mem(out->data(), c.rawLen, stored, c.storedLen, 0);
    return got == c.rawLen;
}

bool Reader::Parse(std::string* err) {
    const uint64_t size = data_.size();
    if (size < kMagicBytes || memcmp(data_.data(), kMagic, 4) != 0) {
        *err = "not a .wsr file";
        return false;
    }
    uint64_t pos = kMagicBytes, indexAt = 0;
    bool sawIndex = false;
    while (pos + kChunkHeaderBytes <= size) {
        const uint8_t* h = data_.data() + pos;
        const ChunkRef c{Rd32(h), Rd32(h + 4), Rd32(h + 8), Rd32(h + 12), pos};
        if ((c.flags & ~kFlagDeflate) || c.rawLen > kMaxChunkBytes || c.storedLen > size - pos - kChunkHeaderBytes) break;
        if (!(c.flags & kFlagDeflate) && c.storedLen != c.rawLen) break;
        if (Crc(h + kChunkHeaderBytes, c.storedLen) != Rd32(h + 16)) break;
        chunks_.push_back(c);
        pos += kChunkHeaderBytes + c.storedLen;
        if (c.type == kINDX) {
            sawIndex = true;
            indexAt = c.offset;
            break;
        }
    }
    if (chunks_.empty() || chunks_[0].type != kHEAD) {
        *err = "no HEAD chunk";
        return false;
    }
    std::vector<uint8_t> p;
    if (!Payload(chunks_[0], &p)) {
        *err = "the HEAD chunk does not decode";
        return false;
    }
    head_.assign(p.begin(), p.end());
    json::Value v;
    json::Error je;
    if (!json::Parse(head_, &v, &je) || !v.IsObject()) {
        *err = "HEAD is not a JSON object";
        return false;
    }
    const json::Value* f = v.Get("format");
    if (!f || !f->IsInteger() || f->number < 1) {
        *err = "HEAD has no format";
        return false;
    }
    if (f->number > kFormat) {
        *err = "format " + std::to_string(static_cast<long long>(f->number)) + " is newer than this reader (" +
               std::to_string(kFormat) + ")";
        return false;
    }
    format_ = static_cast<int>(f->number);
    const json::Value* eh = v.Get("engineHash");
    engineHash_ = eh && eh->IsInteger() && eh->number >= 0 && eh->number < 1e9 ? static_cast<int>(eh->number) : 0;

    if (sawIndex && pos + kTrailerBytes == size && Rd64(data_.data() + pos) == indexAt &&
        memcmp(data_.data() + pos + 8, kEnd, 4) == 0) {
        const ChunkRef& ic = chunks_.back();
        if (ic.flags == 0 && ic.rawLen % kIndexEntryBytes == 0 && ic.rawLen / kIndexEntryBytes == chunks_.size() - 1) {
            const uint8_t* q = data_.data() + ic.offset + kChunkHeaderBytes;
            bool ok = true;
            for (size_t i = 0; i + 1 < chunks_.size(); ++i, q += kIndexEntryBytes) {
                const IndexEntry e{Rd32(q), Rd64(q + 4), Rd32(q + 12), Rd32(q + 16)};
                if (e.type != chunks_[i].type || e.offset != chunks_[i].offset) ok = false;
                index_.push_back(e);
            }
            complete_ = ok;
            if (!ok) index_.clear();
        }
    }
    return true;
}
}  // namespace melange::wormsign::wsr
