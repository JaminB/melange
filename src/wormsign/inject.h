#pragma once
#include <cstdint>
// String inputs: the ReplayMessageStore string sender 0x543130 takes (id, XString*, time). An XString is one pointer
// to refcounted character data; EngineString builds one with the game's own constructor (0x638101) and releases it
// with the game's destructor (0x4030a7), so the store can keep its own reference.
namespace melange::wormsign::inject {
bool Available();                 // both functions hold their build #1077 bytes

class EngineString {
  public:
    explicit EngineString(const char* s);
    ~EngineString();
    EngineString(const EngineString&) = delete;
    EngineString& operator=(const EngineString&) = delete;
    bool Ok() const { return data_ != 0; }
    void* Arg() { return &data_; }    // the sender's second argument
  private:
    uintptr_t data_ = 0;
};
}  // namespace melange::wormsign::inject
