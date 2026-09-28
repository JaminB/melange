#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// RFC 6455 message rules on top of civetweb's frame reader: reassembly, size cap, opcode and UTF-8 checks.
namespace melange::oasis::core::ws {
enum Op : uint8_t { kCont = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

bool Utf8Valid(const char* p, size_t n);

class Assembler {
  public:
    explicit Assembler(size_t maxMessage = 1u << 20) : max_(maxMessage) {}
    enum class Out { None, Message, Close, Control };
    // `bits` is the frame's first byte (FIN, RSV1-3, opcode). On Message, message() holds one complete text
    // message. On Close, closeCode() is the code to answer with (1000 for a client close). On Control, the
    // frame was a ping or pong and needs nothing from the caller.
    Out Feed(uint8_t bits, const char* data, size_t n);
    const std::string& message() const { return msg_; }
    uint16_t closeCode() const { return code_; }

  private:
    Out Fail(uint16_t code);
    std::string msg_;
    size_t max_;
    bool inFragment_ = false;
    uint16_t code_ = 0;
};

// The payload of a close frame: code (big endian) plus a reason truncated to 123 bytes.
std::string ClosePayload(uint16_t code, std::string_view reason);
}  // namespace melange::oasis::core::ws
