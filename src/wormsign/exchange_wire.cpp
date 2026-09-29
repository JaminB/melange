#include "wormsign/exchange_wire.h"

#include <algorithm>
#include <cstring>

#include "wormsign/hash_engine.h"

namespace melange::wormsign::wire {
namespace {
class Out {
  public:
    explicit Out(Kind k) {
        U32(kMagic);
        U8(static_cast<uint8_t>(k));
        U8(kProtocol);
        U16(0);
    }
    void U8(uint8_t v) { b_.push_back(v); }
    void U16(uint16_t v) { Raw(&v, 2); }
    void U32(uint32_t v) { Raw(&v, 4); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void Raw(const void* p, size_t n) {
        auto c = static_cast<const uint8_t*>(p);
        b_.insert(b_.end(), c, c + n);
    }
    void Str(const std::string& s, size_t max) {
        const size_t n = (std::min)(s.size(), max);
        U8(static_cast<uint8_t>(n));
        Raw(s.data(), n);
    }
    size_t Size() const { return b_.size(); }
    std::vector<uint8_t> Done() {
        const uint16_t n = static_cast<uint16_t>(b_.size());
        memcpy(b_.data() + 6, &n, 2);
        return std::move(b_);
    }

  private:
    std::vector<uint8_t> b_;
};

class In {
  public:
    In(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool U8(uint8_t* v) { return Get(v, 1); }
    bool U16(uint16_t* v) { return Get(v, 2); }
    bool U32(uint32_t* v) { return Get(v, 4); }
    bool U64(uint64_t* v) { return Get(v, 8); }
    bool Str(std::string* s, size_t max) {
        uint8_t n = 0;
        if (!U8(&n) || n > max || n_ - o_ < n) return false;
        s->assign(reinterpret_cast<const char*>(p_ + o_), n);
        o_ += n;
        return true;
    }
    bool Bytes(std::vector<uint8_t>* v, size_t n) {
        if (n_ - o_ < n) return false;
        v->assign(p_ + o_, p_ + o_ + n);
        o_ += n;
        return true;
    }
    bool End() const { return o_ == n_; }

  private:
    bool Get(void* v, size_t k) {
        if (n_ - o_ < k) return false;
        memcpy(v, p_ + o_, k);
        o_ += k;
        return true;
    }
    const uint8_t* p_;
    size_t n_, o_ = 0;
};

bool Printable(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}
}  // namespace

std::vector<uint8_t> Encode(const Hello& m) {
    Out o(Kind::Hello);
    o.U32(m.engineHash);
    o.U64(m.matchKey);
    o.U32(m.firstTick);
    o.U64(m.contribHash);
    const size_t flagsAt = o.Size();
    o.U8(0);
    o.Str(m.melange, 23);
    o.Str(m.content16, 16);
    const size_t countAt = o.Size();
    o.U8(0);
    uint8_t n = 0;
    bool truncated = m.namesTruncated;
    for (const ContribName& c : m.contribs) {
        const size_t len = (std::min)(c.name.size(), kMaxNameBytes);
        if (n == 255 || o.Size() + 1 + len + 4 > kMaxPacket) {
            truncated = true;
            break;
        }
        o.Str(c.name, kMaxNameBytes);
        o.U32(c.version);
        ++n;
    }
    std::vector<uint8_t> b = o.Done();
    b[flagsAt] = static_cast<uint8_t>((m.seenYou ? 1 : 0) | (truncated ? 2 : 0));
    b[countAt] = n;
    return b;
}

std::vector<uint8_t> Encode(const Hashes& m) {
    Out o(Kind::Hashes);
    const uint32_t n = (std::min)(static_cast<uint32_t>(m.ticks.size()), kMaxBatch);
    o.U64(m.matchKey);
    o.U32(m.firstTick);
    o.U8(static_cast<uint8_t>(n));
    o.U64(n >= 64 ? m.present : m.present & ((1ull << n) - 1));
    for (uint32_t i = 0; i < n; ++i) {
        o.U64(m.ticks[i].engine);
        o.U64(m.ticks[i].mods);
    }
    return o.Done();
}

std::vector<uint8_t> Encode(Kind reqKind, const TickReq& m) {
    Out o(reqKind);
    o.U64(m.matchKey);
    o.U32(m.tick);
    return o.Done();
}

std::vector<uint8_t> Encode(const Comps& m) {
    Out o(Kind::Comps);
    o.U64(m.matchKey);
    o.U32(m.tick);
    o.U8(m.have ? 1 : 0);
    o.U64(m.engine);
    o.U64(m.mods);
    for (uint64_t c : m.c) o.U64(c);
    const uint32_t n = (std::min)(static_cast<uint32_t>(m.contrib.size()), kMaxCompContribs);
    o.U8(static_cast<uint8_t>(n));
    for (uint32_t i = 0; i < n; ++i) o.U64(m.contrib[i]);
    return o.Done();
}

std::vector<uint8_t> Encode(const DetailChunk& m) {
    Out o(Kind::Detail);
    const size_t n = (std::min)(m.bytes.size(), kDetailChunkBytes);
    o.U64(m.matchKey);
    o.U32(m.tick);
    o.U16(m.index);
    o.U16(m.count);
    o.U32(m.total);
    o.U16(static_cast<uint16_t>(n));
    o.Raw(m.bytes.data(), n);
    return o.Done();
}

std::vector<uint8_t> Encode(const Flag& m) {
    Out o(Kind::Flag);
    o.U64(m.matchKey);
    o.U32(m.tick);
    o.U64(m.engine);
    o.U64(m.mods);
    for (uint64_t c : m.c) o.U64(c);
    return o.Done();
}

std::vector<std::vector<uint8_t>> EncodeDetail(uint64_t matchKey, uint32_t tick, const std::string& json) {
    std::vector<std::vector<uint8_t>> out;
    if (json.empty() || json.size() > kMaxDetailBytes) return out;
    const size_t count = (json.size() + kDetailChunkBytes - 1) / kDetailChunkBytes;
    for (size_t i = 0; i < count; ++i) {
        DetailChunk c;
        c.matchKey = matchKey;
        c.tick = tick;
        c.index = static_cast<uint16_t>(i);
        c.count = static_cast<uint16_t>(count);
        c.total = static_cast<uint32_t>(json.size());
        const size_t at = i * kDetailChunkBytes, n = (std::min)(kDetailChunkBytes, json.size() - at);
        c.bytes.assign(json.begin() + static_cast<ptrdiff_t>(at), json.begin() + static_cast<ptrdiff_t>(at + n));
        out.push_back(Encode(c));
    }
    return out;
}

DecodeResult Decode(const uint8_t* p, size_t n, Packet* out) {
    if (!p || n < kHeaderBytes || n > kMaxPacket) return DecodeResult::NotOurs;
    In in(p, n);
    uint32_t magic = 0;
    uint8_t kind = 0, proto = 0;
    uint16_t len = 0;
    in.U32(&magic);
    in.U8(&kind);
    in.U8(&proto);
    in.U16(&len);
    if (magic != kMagic) return DecodeResult::NotOurs;
    out->protocol = proto;
    if (proto != kProtocol) return DecodeResult::BadProtocol;
    if (len != n || kind < static_cast<uint8_t>(Kind::Hello) || kind > static_cast<uint8_t>(Kind::Flag))
        return DecodeResult::Malformed;
    out->kind = static_cast<Kind>(kind);
    bool ok = false;
    switch (out->kind) {
        case Kind::Hello: {
            Hello& h = out->hello;
            h = Hello{};
            uint8_t flags = 0, count = 0;
            ok = in.U32(&h.engineHash) && in.U64(&h.matchKey) && in.U32(&h.firstTick) && in.U64(&h.contribHash) &&
                 in.U8(&flags) && in.Str(&h.melange, 23) && in.Str(&h.content16, 16) && in.U8(&count) &&
                 Printable(h.melange) && Printable(h.content16);
            h.seenYou = flags & 1;
            h.namesTruncated = (flags & 2) != 0;
            for (uint8_t i = 0; ok && i < count; ++i) {
                ContribName c;
                ok = in.Str(&c.name, kMaxNameBytes) && in.U32(&c.version) && Printable(c.name);
                if (ok) h.contribs.push_back(std::move(c));
            }
            break;
        }
        case Kind::Hashes: {
            Hashes& h = out->hashes;
            h = Hashes{};
            uint8_t count = 0;
            ok = in.U64(&h.matchKey) && in.U32(&h.firstTick) && in.U8(&count) && in.U64(&h.present) && count <= kMaxBatch &&
                 h.firstTick <= 0xffffffffu - count;
            for (uint8_t i = 0; ok && i < count; ++i) {
                TickPair t;
                ok = in.U64(&t.engine) && in.U64(&t.mods);
                h.ticks.push_back(t);
            }
            if (ok && count < 64) h.present &= (1ull << count) - 1;
            break;
        }
        case Kind::CompsReq:
        case Kind::DetailReq:
            out->req = TickReq{};
            ok = in.U64(&out->req.matchKey) && in.U32(&out->req.tick);
            break;
        case Kind::Comps: {
            Comps& c = out->comps;
            c = Comps{};
            uint8_t have = 0, count = 0;
            ok = in.U64(&c.matchKey) && in.U32(&c.tick) && in.U8(&have) && in.U64(&c.engine) && in.U64(&c.mods);
            for (uint64_t& v : c.c) ok = ok && in.U64(&v);
            ok = ok && in.U8(&count) && count <= kMaxCompContribs;
            c.have = have != 0;
            for (uint8_t i = 0; ok && i < count; ++i) {
                uint64_t v = 0;
                ok = in.U64(&v);
                c.contrib.push_back(v);
            }
            break;
        }
        case Kind::Detail: {
            DetailChunk& d = out->detail;
            d = DetailChunk{};
            uint16_t len2 = 0;
            ok = in.U64(&d.matchKey) && in.U32(&d.tick) && in.U16(&d.index) && in.U16(&d.count) && in.U32(&d.total) &&
                 in.U16(&len2) && len2 <= kDetailChunkBytes && in.Bytes(&d.bytes, len2) && d.count > 0 &&
                 d.count <= kMaxDetailChunks && d.index < d.count && d.total > 0 && d.total <= kMaxDetailBytes;
            break;
        }
        case Kind::Flag: {
            Flag& f = out->flag;
            f = Flag{};
            ok = in.U64(&f.matchKey) && in.U32(&f.tick) && in.U64(&f.engine) && in.U64(&f.mods);
            for (uint64_t& v : f.c) ok = ok && in.U64(&v);
            break;
        }
    }
    return ok && in.End() ? DecodeResult::Ok : DecodeResult::Malformed;
}

void DetailAssembly::Reset() { *this = DetailAssembly{}; }

bool DetailAssembly::Add(const DetailChunk& c, std::string* json) {
    if (c.count == 0 || c.index >= c.count || c.total == 0 || c.total > kMaxDetailBytes ||
        static_cast<uint64_t>(c.count) * kDetailChunkBytes < c.total ||
        static_cast<uint64_t>(c.count - 1) * kDetailChunkBytes >= c.total)
        return false;
    if (c.matchKey != key_ || c.tick != tick_ || c.count != count_ || c.total != total_ || buf_.empty()) {
        key_ = c.matchKey;
        tick_ = c.tick;
        count_ = c.count;
        total_ = c.total;
        got_ = 0;
        buf_.assign(total_, 0);
        have_.assign(count_, false);
    }
    const size_t at = static_cast<size_t>(c.index) * kDetailChunkBytes;
    const size_t want = (std::min)(kDetailChunkBytes, static_cast<size_t>(total_) - at);
    if (c.bytes.size() != want || have_[c.index]) return false;
    memcpy(buf_.data() + at, c.bytes.data(), want);
    have_[c.index] = true;
    if (++got_ != count_) return false;
    json->assign(buf_.begin(), buf_.end());
    buf_.clear();
    have_.clear();
    got_ = 0;
    return true;
}

uint64_t ContribListHash(const std::vector<ContribName>& names) {
    uint64_t h = kFnvBasis;
    for (const ContribName& c : names) {
        h = Fnv(c.name.data(), c.name.size(), h);
        h = FnvV(static_cast<uint8_t>(0), h);
        h = FnvV(c.version, h);
    }
    return h;
}

uint64_t MatchKey(uint64_t hostSteamId, uint64_t lobby) {
    const uint64_t h = FnvV(lobby, FnvV(hostSteamId));
    return h ? h : 1;
}
}  // namespace melange::wormsign::wire
