#include "oasis/core/ws.h"

namespace melange::oasis::core::ws {
bool Utf8Valid(const char* s, size_t n) {
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        int len;
        uint32_t cp;
        if (c >= 0xc2 && c <= 0xdf) { len = 2; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { len = 3; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + static_cast<size_t>(len) > n) return false;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = p[i + static_cast<size_t>(k)];
            if ((cc & 0xc0) != 0x80) return false;
            cp = cp << 6 | (cc & 0x3f);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        i += static_cast<size_t>(len);
    }
    return true;
}

Assembler::Out Assembler::Fail(uint16_t code) {
    code_ = code;
    msg_.clear();
    inFragment_ = false;
    return Out::Close;
}

Assembler::Out Assembler::Feed(uint8_t bits, const char* data, size_t n) {
    const bool fin = bits & 0x80;
    const uint8_t op = bits & 0x0f;
    if (bits & 0x70) return Fail(1002);
    if (op >= 8) {
        if (!fin || n > 125) return Fail(1002);
        if (op == kClose) {
            if (n == 1) return Fail(1002);
            if (n >= 2) {
                const uint16_t c = static_cast<uint16_t>(static_cast<unsigned char>(data[0]) << 8 | static_cast<unsigned char>(data[1]));
                const bool valid = (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1011) || (c >= 3000 && c <= 4999);
                if (!valid || !Utf8Valid(data + 2, n - 2)) return Fail(1002);
            }
            return Fail(1000);
        }
        if (op == kPing || op == kPong) return Out::Control;
        return Fail(1002);
    }
    if (op == kBinary) return Fail(1003);
    if (op == kText) {
        if (inFragment_) return Fail(1002);
        msg_.clear();
        inFragment_ = true;
    } else if (op == kCont) {
        if (!inFragment_) return Fail(1002);
    } else {
        return Fail(1002);
    }
    if (msg_.size() + n > max_) return Fail(1009);
    msg_.append(data, n);
    if (!fin) return Out::None;
    inFragment_ = false;
    if (!Utf8Valid(msg_.data(), msg_.size())) return Fail(1007);
    return Out::Message;
}

std::string ClosePayload(uint16_t code, std::string_view reason) {
    std::string p;
    p += static_cast<char>(code >> 8);
    p += static_cast<char>(code & 0xff);
    if (reason.size() > 123) reason = reason.substr(0, 123);
    p += reason;
    return p;
}
}  // namespace melange::oasis::core::ws
