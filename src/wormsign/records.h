#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// Binary layouts for the streamed .wsr chunk payloads (INPT/RMTI/DISP/SEED/PDRW/TICK). format.h only knows chunk
// framing; the record shapes inside a chunk's payload are the recorder's own choice, kept here so the recorder
// (writer side) and the library (reader side, for counts) agree on one definition.
namespace melange::wormsign::records {

// INPT: {type u8, id u16, a u32, b u32, time u32, callT u32, caller u32, strLen u8, str[strLen]}
inline void AppendInput(std::vector<uint8_t>& out, uint8_t type, uint16_t id, uint32_t a, uint32_t b, uint32_t time,
                        uint32_t callT, uint32_t caller, const char* str) {
    const uint8_t strLen = str ? static_cast<uint8_t>(strnlen(str, 255)) : 0;
    const size_t base = out.size();
    out.resize(base + 1 + 2 + 4 + 4 + 4 + 4 + 4 + 1 + strLen);
    uint8_t* p = out.data() + base;
    *p = type, p += 1;
    memcpy(p, &id, 2), p += 2;
    memcpy(p, &a, 4), p += 4;
    memcpy(p, &b, 4), p += 4;
    memcpy(p, &time, 4), p += 4;
    memcpy(p, &callT, 4), p += 4;
    memcpy(p, &caller, 4), p += 4;
    *p = strLen, p += 1;
    if (strLen) memcpy(p, str, strLen);
}
// fn(type, id, a, b, time, callT, caller, str, strLen). Returns the record count, or -1 if the buffer is malformed.
template <class Fn>
int64_t ForEachInput(const uint8_t* data, size_t n, Fn&& fn) {
    size_t i = 0;
    int64_t count = 0;
    while (i < n) {
        if (i + 24 > n) return -1;
        const uint8_t type = data[i];
        uint16_t id;
        uint32_t a, b, time, callT, caller;
        memcpy(&id, data + i + 1, 2);
        memcpy(&a, data + i + 3, 4);
        memcpy(&b, data + i + 7, 4);
        memcpy(&time, data + i + 11, 4);
        memcpy(&callT, data + i + 15, 4);
        memcpy(&caller, data + i + 19, 4);
        const uint8_t strLen = data[i + 23];
        if (i + 24 + strLen > n) return -1;
        fn(type, id, a, b, time, callT, caller, reinterpret_cast<const char*>(data + i + 24), strLen);
        i += 24 + strLen;
        ++count;
    }
    return count;
}

// RMTI: {arrivedT u32, id u16, time u32, a u32} -- fixed 14 bytes.
constexpr size_t kRemoteInputBytes = 14;
inline void AppendRemoteInput(std::vector<uint8_t>& out, uint32_t arrivedT, uint16_t id, uint32_t time, uint32_t a) {
    const size_t base = out.size();
    out.resize(base + kRemoteInputBytes);
    uint8_t* p = out.data() + base;
    memcpy(p, &arrivedT, 4), p += 4;
    memcpy(p, &id, 2), p += 2;
    memcpy(p, &time, 4), p += 4;
    memcpy(p, &a, 4);
}
template <class Fn>
int64_t ForEachRemoteInput(const uint8_t* data, size_t n, Fn&& fn) {
    if (n % kRemoteInputBytes) return -1;
    const size_t count = n / kRemoteInputBytes;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = data + i * kRemoteInputBytes;
        uint32_t arrivedT, time, a;
        uint16_t id;
        memcpy(&arrivedT, p, 4);
        memcpy(&id, p + 4, 2);
        memcpy(&time, p + 6, 4);
        memcpy(&a, p + 10, 4);
        fn(arrivedT, id, time, a);
    }
    return static_cast<int64_t>(count);
}

// DISP (diagnostic): {t u32, id u16} -- fixed 6 bytes.
constexpr size_t kDispatchBytes = 6;
inline void AppendDispatch(std::vector<uint8_t>& out, uint32_t t, uint16_t id) {
    const size_t base = out.size();
    out.resize(base + kDispatchBytes);
    memcpy(out.data() + base, &t, 4);
    memcpy(out.data() + base + 4, &id, 2);
}

// SEED: {kind u8, value u32, caller u32, t u32} -- fixed 13 bytes.
constexpr size_t kSeedBytes = 13;
inline void AppendSeed(std::vector<uint8_t>& out, uint8_t kind, uint32_t value, uint32_t caller, uint32_t t) {
    const size_t base = out.size();
    out.resize(base + kSeedBytes);
    uint8_t* p = out.data() + base;
    *p = kind, p += 1;
    memcpy(p, &value, 4), p += 4;
    memcpy(p, &caller, 4), p += 4;
    memcpy(p, &t, 4);
}

// PDRW: {rng u8, ret u32, stateAfter u32, bits u32} -- fixed 13 bytes.
constexpr size_t kPreDrawBytes = 13;
inline void AppendPreDraw(std::vector<uint8_t>& out, uint8_t rng, uint32_t ret, uint32_t stateAfter, uint32_t bits) {
    const size_t base = out.size();
    out.resize(base + kPreDrawBytes);
    uint8_t* p = out.data() + base;
    *p = rng, p += 1;
    memcpy(p, &ret, 4), p += 4;
    memcpy(p, &stateAfter, 4), p += 4;
    memcpy(p, &bits, 4);
}

// TICK: a TickHash (melange/wormsign.h) without its leading `tick` field -- implicit and consecutive within the
// chunk's [tickFrom, tickTo] index range. engine u64, mods u64, c[6] u64, rngLogic u32, rng2 u32, fpucw u16,
// inputs u16 -- 8+8+48+4+4+2+2 = 76 bytes.
constexpr size_t kTickBytes = 76;
}  // namespace melange::wormsign::records
