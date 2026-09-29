#include "wormsign/inject.h"

#include "core/mem.h"

namespace melange::wormsign::inject {
namespace {
constexpr uintptr_t kCtor = 0x638101;   // XString::XString(const char*), thiscall
constexpr uintptr_t kDtor = 0x4030a7;   // XString::~XString(), thiscall
using Ctor = void(__thiscall*)(void* self, const char* s);
using Dtor = void(__thiscall*)(void* self);

int g_ok = -1;
}  // namespace

bool Available() {
    if (g_ok < 0)
        g_ok = mem::Expect(kCtor, {0x55, 0x8b, 0x6c, 0x24, 0x08, 0x57, 0x8b, 0xf9, 0x85, 0xed}) &&
               mem::Expect(kDtor, {0x55, 0x8b, 0xec, 0x51, 0x89, 0x4d, 0xfc, 0x8b, 0x4d, 0xfc, 0xe8});
    return g_ok == 1;
}

EngineString::EngineString(const char* s) {
    if (!Available()) return;
    reinterpret_cast<Ctor>(kCtor)(&data_, s ? s : "");
}

EngineString::~EngineString() {
    if (data_) reinterpret_cast<Dtor>(kDtor)(&data_);
}
}  // namespace melange::wormsign::inject
