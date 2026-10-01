#pragma once
// GL_TIME_ELAPSED queries for the swap-to-swap frame and each Mirage stage (L0 of the graphics plan). Region
// indices match melange::gltrace::GpuRegion 1:1 (0 = swap, 1..5 = render::Stage World..Final). No hub/hook
// dependency: plain GL 3.3 / GL_ARB_timer_query, resolved lazily against whatever context is current.
namespace melange::mirage::gputimers {
bool Supported();        // a context is current and exposes timer queries
void SetEnabled(bool on);  // [Mirage] GpuTimers; false skips the GL calls entirely
bool Enabled();
void Begin(int region);  // main thread, GL context current; one call per region per frame
void End(int region);
}  // namespace melange::mirage::gputimers
