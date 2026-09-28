#pragma once
#include <cstdint>
namespace melange::gldebug {
bool DebugContext();  // the game's context was created with CONTEXT_DEBUG_BIT
struct Stats { uint64_t messages, suppressed; uint32_t high, medium, low, notification; };
Stats GetStats();
// KHR_debug groups and labels for Melange's own GL work (no-ops without KHR_debug).
void PushGroup(const char* label);
void PopGroup();
void Label(unsigned identifier, unsigned name, const char* label);  // e.g. GL_TEXTURE, GL_FRAMEBUFFER
}
