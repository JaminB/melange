#pragma once
#include <cstdint>
// The six ReplayMessageStore sender hooks and five insert hooks. Sender hooks call one gate.
namespace melange::wormsign::capture {
enum class Gate : uint8_t { Pass, Block };
using GateFn = Gate (*)(int type, uint16_t id, bool injected);  // the player installs it while a replay plays
void SetGate(GateFn fn);
bool InjectSend(int type, uint16_t id, uint32_t a, uint32_t b, const char* str, uint32_t time);  // marks injected
bool LocalOnly(uint16_t id);              // bsearch of the 0x91eef8 table, as 0x539c00
}  // namespace melange::wormsign::capture
