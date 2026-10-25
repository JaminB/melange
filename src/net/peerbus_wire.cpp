#include "net/peerbus_wire.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace melange::peerbus::wire {
namespace {
class Out {
  public:
    explicit Out(std::vector<uint8_t>* b) : b_(b) {}
    void U8(uint8_t v) { b_->push_back(v); }
    void U16(uint16_t v) { Raw(&v, 2); }
    void U32(uint32_t v) { Raw(&v, 4); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void F64(double v) { Raw(&v, 8); }
    void Var(uint64_t v) {
        while (v >= 0x80) {
            U8(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        U8(static_cast<uint8_t>(v));
    }
    void Raw(const void* p, size_t n) {
        auto c = static_cast<const uint8_t*>(p);
        b_->insert(b_->end(), c, c + n);
    }

  private:
    std::vector<uint8_t>* b_;
};

class In {
  public:
    In(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool U8(uint8_t* v) { return Get(v, 1); }
    bool U16(uint16_t* v) { return Get(v, 2); }
    bool U32(uint32_t* v) { return Get(v, 4); }
    bool U64(uint64_t* v) { return Get(v, 8); }
    bool F64(double* v) { return Get(v, 8); }
    bool Var(uint64_t* v) {
        uint64_t r = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            uint8_t b = 0;
            if (!U8(&b)) return false;
            if (shift == 63 && b > 1) return false;
            r |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) {
                *v = r;
                return true;
            }
        }
        return false;
    }
    bool Str(std::string* s, size_t len) {
        if (n_ - o_ < len) return false;
        s->assign(reinterpret_cast<const char*>(p_ + o_), len);
        o_ += len;
        return true;
    }
    size_t Left() const { return n_ - o_; }
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

bool IdChar(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

bool KnownKind(uint8_t k) { return k >= static_cast<uint8_t>(Kind::Hello) && k <= static_cast<uint8_t>(Kind::Bye); }

// ---------------------------------------------------------------- values
enum Tag : uint8_t { kNil = 0, kFalse, kTrue, kInt, kNum, kStr, kTable };

bool ValidKey(const Value& k) {
    switch (k.type) {
        case Value::Type::Bool:
        case Value::Type::Int:
        case Value::Type::Str: return true;
        case Value::Type::Num: return !std::isnan(k.n);
        default: return false;
    }
}

bool Put(const Value& v, Out& o, int depth) {
    switch (v.type) {
        case Value::Type::Nil: o.U8(kNil); return true;
        case Value::Type::Bool: o.U8(v.b ? kTrue : kFalse); return true;
        case Value::Type::Int:
            o.U8(kInt);
            o.Var((static_cast<uint64_t>(v.i) << 1) ^ static_cast<uint64_t>(v.i >> 63));
            return true;
        case Value::Type::Num:
            o.U8(kNum);
            o.F64(v.n);
            return true;
        case Value::Type::Str:
            if (v.s.size() > kMaxString) return false;
            o.U8(kStr);
            o.Var(v.s.size());
            o.Raw(v.s.data(), v.s.size());
            return true;
        case Value::Type::Table:
            if (depth >= kMaxDepth || v.t.size() > kMaxKeys) return false;
            o.U8(kTable);
            o.Var(v.t.size());
            for (const auto& [k, x] : v.t)
                if (!ValidKey(k) || k.type == Value::Type::Table || !Put(k, o, depth + 1) || !Put(x, o, depth + 1)) return false;
            return true;
    }
    return false;
}

bool Get(In& in, Value* v, int depth) {
    uint8_t tag = 0;
    if (!in.U8(&tag)) return false;
    switch (tag) {
        case kNil: v->type = Value::Type::Nil; return true;
        case kFalse:
        case kTrue:
            v->type = Value::Type::Bool;
            v->b = tag == kTrue;
            return true;
        case kInt: {
            uint64_t z = 0;
            if (!in.Var(&z)) return false;
            v->type = Value::Type::Int;
            v->i = static_cast<int64_t>(z >> 1) ^ -static_cast<int64_t>(z & 1);
            return true;
        }
        case kNum:
            v->type = Value::Type::Num;
            return in.F64(&v->n);
        case kStr: {
            uint64_t len = 0;
            if (!in.Var(&len) || len > kMaxString) return false;
            v->type = Value::Type::Str;
            return in.Str(&v->s, static_cast<size_t>(len));
        }
        case kTable: {
            uint64_t count = 0;
            // depth counts nesting the same way Put does: the top table is depth 0, so at most kMaxDepth levels.
            if (depth >= kMaxDepth || !in.Var(&count) || count > kMaxKeys || count * 2 > in.Left()) return false;
            v->type = Value::Type::Table;
            v->t.reserve(static_cast<size_t>(count));
            for (uint64_t i = 0; i < count; ++i) {
                Value k, x;
                if (!Get(in, &k, depth + 1) || !ValidKey(k) || !Get(in, &x, depth + 1)) return false;
                v->t.emplace_back(std::move(k), std::move(x));
            }
            return true;
        }
        default: return false;
    }
}
}  // namespace

size_t MaxMessage(size_t modIdLen, bool reliable) {
    if (modIdLen > kMaxModId) return 0;
    const size_t chunk = ChunkBytes(modIdLen);
    return reliable ? (std::min)(kMaxReliable, chunk * kMaxFrags) : (std::min)(kMaxUnreliable, chunk);
}

bool ValidModId(std::string_view id) {
    if (id.empty() || id.size() > kMaxModId) return false;
    for (unsigned char c : id)
        if (!IdChar(c)) return false;
    return true;
}

std::vector<uint8_t> Encode(const Header& h, const uint8_t* body, size_t n) {
    std::vector<uint8_t> b;
    if (!h.modId.empty() && !ValidModId(h.modId)) return b;
    const size_t total = HeaderBytes(h.modId.size()) + n;
    if (total > kMaxPacket) return b;
    b.reserve(total);
    Out o(&b);
    o.U32(kMagic);
    o.U8(static_cast<uint8_t>(static_cast<uint8_t>(h.kind) | (h.reliable ? kReliableBit : 0)));
    o.U8(h.protocol);
    o.U16(static_cast<uint16_t>(total));
    o.U64(h.lobbyKey);
    o.U8(static_cast<uint8_t>(h.modId.size()));
    o.Raw(h.modId.data(), h.modId.size());
    o.U16(h.seq);
    o.U8(h.frag);
    o.U8(h.nfrag);
    if (n) o.Raw(body, n);
    return b;
}

std::vector<std::vector<uint8_t>> EncodeMessage(uint64_t lobbyKey, const std::string& modId, uint16_t seq, bool reliable,
                                                const std::vector<uint8_t>& payload) {
    std::vector<std::vector<uint8_t>> out;
    if (!ValidModId(modId) || payload.size() > MaxMessage(modId.size(), reliable)) return out;
    const size_t chunk = ChunkBytes(modId.size());
    const size_t nfrag = payload.empty() ? 1 : (payload.size() + chunk - 1) / chunk;
    Header h;
    h.kind = Kind::Msg;
    h.reliable = reliable;
    h.lobbyKey = lobbyKey;
    h.modId = modId;
    h.seq = seq;
    h.nfrag = static_cast<uint8_t>(nfrag);
    for (size_t i = 0; i < nfrag; ++i) {
        h.frag = static_cast<uint8_t>(i);
        const size_t at = i * chunk, len = (std::min)(chunk, payload.size() - at);
        out.push_back(Encode(h, payload.data() + at, len));
    }
    return out;
}

DecodeResult Decode(const uint8_t* p, size_t n, Packet* out) {
    if (!p || n < kFixedHeader || n > kMaxPacket) return DecodeResult::NotOurs;
    In in(p, n);
    uint32_t magic = 0;
    uint8_t kind = 0, proto = 0, idLen = 0;
    uint16_t len = 0;
    in.U32(&magic);
    if (magic != kMagic) return DecodeResult::NotOurs;
    in.U8(&kind);
    in.U8(&proto);
    in.U16(&len);
    if (len != n) return DecodeResult::NotOurs;
    out->h.protocol = proto;
    if (proto != kProtocol) return DecodeResult::BadProtocol;
    out->h.reliable = (kind & kReliableBit) != 0;
    kind = static_cast<uint8_t>(kind & ~kReliableBit);
    if (!KnownKind(kind)) return DecodeResult::Malformed;
    out->h.kind = static_cast<Kind>(kind);
    in.U64(&out->h.lobbyKey);
    in.U8(&idLen);
    if (idLen > kMaxModId || !in.Str(&out->h.modId, idLen)) return DecodeResult::Malformed;
    if (!in.U16(&out->h.seq) || !in.U8(&out->h.frag) || !in.U8(&out->h.nfrag)) return DecodeResult::Malformed;
    const Kind k = out->h.kind;
    const bool control = k == Kind::Ping || k == Kind::Pong;
    if (control ? idLen != 0 : !ValidModId(out->h.modId)) return DecodeResult::Malformed;
    const uint8_t nf = out->h.nfrag, f = out->h.frag;
    if (nf == 0 || nf > kMaxFrags || f >= nf) return DecodeResult::Malformed;
    if (nf > 1 && (k != Kind::Msg || !out->h.reliable)) return DecodeResult::Malformed;
    out->body.assign(p + HeaderBytes(idLen), p + n);
    const size_t b = out->body.size();
    switch (k) {
        case Kind::Ping: if (b != 8) return DecodeResult::Malformed; break;
        case Kind::Pong: if (b != 24) return DecodeResult::Malformed; break;
        case Kind::Hello: if (b != 2) return DecodeResult::Malformed; break;
        case Kind::Bye: if (b != 0) return DecodeResult::Malformed; break;
        case Kind::Msg:
            if (!out->h.reliable && b > kMaxUnreliable) return DecodeResult::Malformed;
            break;
    }
    return DecodeResult::Ok;
}

std::vector<uint8_t> PingBody(double t0) {
    std::vector<uint8_t> b;
    Out(&b).F64(t0);
    return b;
}
std::vector<uint8_t> PongBody(double t0, double t1, double t2) {
    std::vector<uint8_t> b;
    Out o(&b);
    o.F64(t0);
    o.F64(t1);
    o.F64(t2);
    return b;
}
bool ParsePing(const std::vector<uint8_t>& b, double* t0) {
    In in(b.data(), b.size());
    return in.F64(t0) && in.End() && std::isfinite(*t0);
}
bool ParsePong(const std::vector<uint8_t>& b, double* t0, double* t1, double* t2) {
    In in(b.data(), b.size());
    return in.F64(t0) && in.F64(t1) && in.F64(t2) && in.End() && std::isfinite(*t0) && std::isfinite(*t1) &&
           std::isfinite(*t2);
}
std::vector<uint8_t> HelloBody(uint16_t version) {
    std::vector<uint8_t> b;
    Out(&b).U16(version);
    return b;
}
bool ParseHello(const std::vector<uint8_t>& b, uint16_t* version) {
    In in(b.data(), b.size());
    return in.U16(version) && in.End();
}

bool EncodeValue(const Value& v, std::vector<uint8_t>* out, std::string* why) {
    out->clear();
    Out o(out);
    if (Put(v, o, 0)) return true;
    out->clear();
    if (why) *why = "size";
    return false;
}

bool DecodeValue(const uint8_t* p, size_t n, Value* out) {
    if (!p || !n) return false;
    In in(p, n);
    *out = Value{};
    return Get(in, out, 0) && in.End();
}

// ---------------------------------------------------------------- reassembly
void Reassembler::Erase(std::map<Key, Part>::iterator it) {
    bytes_ -= it->second.bytes;
    parts_.erase(it);
}

Reassembler::Result Reassembler::Add(uint64_t from, const Header& h, const std::vector<uint8_t>& body, size_t chunkMax,
                                     double nowMs, std::vector<uint8_t>* message) {
    if (h.nfrag <= 1) {
        *message = body;
        return Result::Complete;
    }
    if (h.nfrag > kMaxFrags || h.frag >= h.nfrag || body.size() > chunkMax || body.empty()) {
        ++dropped_;
        return Result::Dropped;
    }
    const bool last = h.frag + 1 == h.nfrag;
    if (!last && body.size() != chunkMax) {
        ++dropped_;
        return Result::Dropped;
    }
    const Key key{from, h.modId, h.seq};
    if (done_.count(key)) return Result::Pending;  // a duplicate of a message already delivered
    auto it = parts_.find(key);
    if (it == parts_.end()) {
        size_t mine = 0;
        for (const auto& [k, p] : parts_)
            if (k.from == from) ++mine;
        if (mine >= kMaxPartialPerSender || bytes_ + body.size() > kMaxPartialBytes) {
            ++dropped_;
            return Result::Dropped;
        }
        Part p;
        p.started = nowMs;
        p.nfrag = h.nfrag;
        p.frags.resize(h.nfrag);
        p.have.assign(h.nfrag, false);
        it = parts_.emplace(key, std::move(p)).first;
    }
    Part& p = it->second;
    if (p.nfrag != h.nfrag) {  // the same seq with another shape: the sender is confused or hostile
        Erase(it);
        ++dropped_;
        return Result::Dropped;
    }
    if (p.have[h.frag]) return Result::Pending;  // a duplicate
    if (bytes_ + body.size() > kMaxPartialBytes) {
        Erase(it);
        ++dropped_;
        return Result::Dropped;
    }
    p.frags[h.frag] = body;
    p.have[h.frag] = true;
    p.bytes += body.size();
    bytes_ += body.size();
    if (++p.got != p.nfrag) return Result::Pending;
    message->clear();
    for (const auto& f : p.frags) message->insert(message->end(), f.begin(), f.end());
    Erase(it);
    if (done_.size() >= kMaxDone) {
        auto oldest = done_.begin();
        for (auto d = done_.begin(); d != done_.end(); ++d)
            if (d->second < oldest->second) oldest = d;
        done_.erase(oldest);
    }
    done_[key] = nowMs;
    if (message->size() > kMaxReliable) {
        ++dropped_;
        return Result::Dropped;
    }
    return Result::Complete;
}

void Reassembler::Expire(double nowMs) {
    for (auto it = parts_.begin(); it != parts_.end();) {
        if (nowMs - it->second.started > kTimeoutMs) {
            auto dead = it++;
            Erase(dead);
            ++dropped_;
        } else {
            ++it;
        }
    }
    for (auto it = done_.begin(); it != done_.end();)
        it = nowMs - it->second > kTimeoutMs ? done_.erase(it) : std::next(it);
}

void Reassembler::DropSender(uint64_t from) {
    for (auto it = parts_.begin(); it != parts_.end();) {
        if (it->first.from == from) {
            auto dead = it++;
            Erase(dead);
        } else {
            ++it;
        }
    }
    for (auto it = done_.begin(); it != done_.end();) it = it->first.from == from ? done_.erase(it) : std::next(it);
}

void Reassembler::Clear() {
    parts_.clear();
    done_.clear();
    bytes_ = 0;
}

// ---------------------------------------------------------------- rate limits
void TokenBucket::Configure(double perSec, double cap) {
    rate_ = perSec;
    cap_ = cap;
    if (tokens_ > cap_) tokens_ = cap_;
}

void TokenBucket::Refill(double nowMs) {
    if (!started_) {
        started_ = true;
        last_ = nowMs;
        tokens_ = cap_;
        return;
    }
    if (nowMs > last_) {
        tokens_ = (std::min)(cap_, tokens_ + (nowMs - last_) * rate_ / 1000.0);
        last_ = nowMs;
    }
}

bool TokenBucket::Can(double n, double nowMs) {
    Refill(nowMs);
    return tokens_ >= n;
}

void TokenBucket::Take(double n, double nowMs) {
    Refill(nowMs);
    tokens_ -= n;
}

double TokenBucket::Tokens(double nowMs) {
    Refill(nowMs);
    return tokens_;
}

void GlobalBudget::SetDestinations(size_t n) {
    n_ = n ? n : 1;
    const double rate = kGlobalBytesPerSec / static_cast<double>(n_);
    for (auto& [d, b] : per_) b.Configure(rate, (std::max)(rate, kDestMinBurst));
}

TokenBucket& GlobalBudget::For(uint64_t dest) {
    auto it = per_.find(dest);
    if (it != per_.end()) return it->second;
    const double rate = kGlobalBytesPerSec / static_cast<double>(n_);
    return per_.emplace(dest, TokenBucket(rate, (std::max)(rate, kDestMinBurst))).first->second;
}

bool GlobalBudget::Can(uint64_t dest, size_t wireBytes, double nowMs) {
    const double n = static_cast<double>(wireBytes);
    return total_.Can(n, nowMs) && For(dest).Can(n, nowMs);
}

void GlobalBudget::Take(uint64_t dest, size_t wireBytes, double nowMs) {
    const double n = static_cast<double>(wireBytes);
    total_.Take(n, nowMs);
    For(dest).Take(n, nowMs);
}

// ---------------------------------------------------------------- clock
bool NtpSample(double t0, double t1, double t2, double t3, ClockSample* out) {
    if (!std::isfinite(t0) || !std::isfinite(t1) || !std::isfinite(t2) || !std::isfinite(t3)) return false;
    if (t3 < t0 || t2 < t1) return false;
    const double rtt = (t3 - t0) - (t2 - t1);
    if (rtt < 0 || rtt > ClockEstimator::kMaxRttMs) return false;
    out->rtt = rtt;
    out->offset = ((t1 - t0) + (t2 - t3)) / 2.0;
    return true;
}

bool ClockEstimator::Add(double t0, double t1, double t2, double t3) {
    ClockSample s;
    if (!NtpSample(t0, t1, t2, t3, &s)) return false;
    ring_[next_] = s;
    next_ = (next_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
    rtt_ = total_ == 0 ? s.rtt : kEwma * rtt_ + (1.0 - kEwma) * s.rtt;
    ++total_;
    return true;
}

double ClockEstimator::Offset() const {
    int best = -1;
    for (int i = 0; i < count_; ++i)
        if (best < 0 || ring_[i].rtt < ring_[best].rtt) best = i;
    return best < 0 ? 0 : ring_[best].offset;
}

void ClockEstimator::Reset() {
    count_ = next_ = total_ = 0;
    rtt_ = 0;
}

// ---------------------------------------------------------------- advert
std::string BuildAdvert(std::vector<AdvertEntry> entries) {
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    if (entries.empty()) return "";
    std::string s = "1;";
    for (size_t i = 0; i < entries.size() && i < kMaxAdvertEntries; ++i) {
        if (i) s += ',';
        s += entries[i].modId + "@" + std::to_string(entries[i].version);
    }
    return s;
}

bool ParseAdvert(std::string_view value, std::vector<AdvertEntry>* out) {
    out->clear();
    if (value.size() < 2 || value.substr(0, 2) != "1;") return false;
    std::string_view rest = value.substr(2);
    while (!rest.empty() && out->size() < kMaxAdvertEntries) {
        const size_t comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        const size_t at = item.find('@');
        if (at == std::string_view::npos) continue;
        const std::string_view id = item.substr(0, at), ver = item.substr(at + 1);
        if (!ValidModId(id) || ver.empty() || ver.size() > 5) continue;
        int v = 0;
        bool digits = true;
        for (char c : ver) {
            if (c < '0' || c > '9') digits = false;
            else v = v * 10 + (c - '0');
        }
        if (!digits || v < 1 || v > 65535) continue;
        out->push_back({std::string(id), v});
    }
    return true;
}

bool Advertises(std::string_view value, std::string_view modId, int version) {
    std::vector<AdvertEntry> e;
    if (!ParseAdvert(value, &e)) return false;
    for (const AdvertEntry& a : e)
        if (a.modId == modId && (version < 0 || a.version == version)) return true;
    return false;
}

SendStatus Gate(const GateIn& g) {
    if (!g.channelOpen) return SendStatus::Closed;
    if (!g.online) return SendStatus::Offline;
    if (!g.member || !g.advertises) return SendStatus::NoPeer;
    if (!g.session) return SendStatus::NoSession;
    return SendStatus::Ok;
}
}  // namespace melange::peerbus::wire
