#include "gameplay/crashfix.h"

namespace melange::crashfix {
bool NullToSink(uintptr_t& reg, Sink& sink) {
    if (reg) return false;
    reg = reinterpret_cast<uintptr_t>(sink.bytes);
    return true;
}
}  // namespace melange::crashfix
