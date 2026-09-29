#include "wormsign/records.h"

namespace melange::wormsign::rec {
namespace {
void Le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void Le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void Le64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
uint16_t Rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
}  // namespace

namespace detail {
uint32_t Rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
uint64_t Rd64(const uint8_t* p) { return Rd32(p) | static_cast<uint64_t>(Rd32(p + 4)) << 32; }
void ReadTick(const uint8_t* p, TickHash* h) {
    h->engine = Rd64(p);
    h->mods = Rd64(p + 8);
    for (int i = 0; i < kEngineComps; ++i) h->c[i] = Rd64(p + 16 + 8 * i);
    h->rngLogic = Rd32(p + 64);
    h->rng2 = Rd32(p + 68);
    h->fpucw = Rd16(p + 72);
    h->inputs = Rd16(p + 74);
}
}  // namespace detail

using detail::Rd32;

void AppendSeed(std::vector<uint8_t>& out, const Seed& s) {
    out.push_back(s.kind);
    Le32(out, s.value);
    Le32(out, s.caller);
    Le32(out, s.t);
}

void AppendDraw(std::vector<uint8_t>& out, const Draw& d) {
    out.push_back(d.rng);
    Le32(out, d.ret);
    Le32(out, d.stateAfter);
    Le32(out, d.bits);
}

void AppendInput(std::vector<uint8_t>& out, const Input& i) {
    out.push_back(i.type);
    Le16(out, i.id);
    Le32(out, i.a);
    Le32(out, i.b);
    Le32(out, i.time);
    Le32(out, i.callT);
    Le32(out, i.caller);
    const size_t n = i.str.size() < 255 ? i.str.size() : 255;
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), i.str.begin(), i.str.begin() + static_cast<std::ptrdiff_t>(n));
}

void TickChunk::Add(const TickHash& h) {
    if (bytes_.empty()) {
        first_ = next_ = h.tick;
        count_ = 0;
        Le32(bytes_, h.tick);
    } else if (h.tick < next_) {
        return;
    } else if (h.tick > next_) {
        bytes_.push_back(kTickGap);
        Le32(bytes_, h.tick - next_);
    }
    bytes_.push_back(kTickRec);
    Le64(bytes_, h.engine);
    Le64(bytes_, h.mods);
    for (int i = 0; i < kEngineComps; ++i) Le64(bytes_, h.c[i]);
    Le32(bytes_, h.rngLogic);
    Le32(bytes_, h.rng2);
    Le16(bytes_, h.fpucw);
    Le16(bytes_, h.inputs);
    next_ = h.tick + 1;
    ++count_;
}

std::vector<uint8_t> TickChunk::Take() {
    std::vector<uint8_t> v;
    v.swap(bytes_);
    count_ = 0;
    return v;
}

bool DecodeSeeds(const uint8_t* p, size_t n, std::vector<Seed>* out) {
    for (size_t at = 0; at + kSeedBytes <= n; at += kSeedBytes)
        out->push_back(Seed{p[at], Rd32(p + at + 1), Rd32(p + at + 5), Rd32(p + at + 9)});
    return n % kSeedBytes == 0;
}

bool DecodeDraws(const uint8_t* p, size_t n, std::vector<Draw>* out) {
    for (size_t at = 0; at + kDrawBytes <= n; at += kDrawBytes)
        out->push_back(Draw{p[at], Rd32(p + at + 1), Rd32(p + at + 5), Rd32(p + at + 9)});
    return n % kDrawBytes == 0;
}

bool DecodeInputs(const uint8_t* p, size_t n, std::vector<Input>* out) {
    size_t at = 0;
    while (at < n) {
        if (n - at < kInputFixedBytes) return false;
        const uint8_t* q = p + at;
        Input i;
        i.type = q[0];
        i.id = Rd16(q + 1);
        i.a = Rd32(q + 3);
        i.b = Rd32(q + 7);
        i.time = Rd32(q + 11);
        i.callT = Rd32(q + 15);
        i.caller = Rd32(q + 19);
        const size_t len = q[23];
        if (n - at - kInputFixedBytes < len) return false;
        i.str.assign(reinterpret_cast<const char*>(q + kInputFixedBytes), len);
        out->push_back(std::move(i));
        at += kInputFixedBytes + len;
    }
    return true;
}
}  // namespace melange::wormsign::rec
