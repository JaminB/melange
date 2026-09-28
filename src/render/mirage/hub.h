#pragma once
// One IAT/proc interposition layer for the game's OpenGL calls, shared by the Mirage components.
#include <cstdint>

namespace melange::mirage::hub {
enum class Src : uint8_t { ExeIat, ExeProc, CgGLIat, CgGLProc };
enum class Mode : uint8_t { Passthrough, Count, Log };
// Call from Module::Install only (before the game creates its context). Idempotent. Installs the thunks for the
// exe and cgGL.dll OPENGL32 imports and both modules' wglGetProcAddress. Nothing is patched until someone calls it.
bool Require(const char* who);
bool Installed();
// Chains a typed wrapper in front of a GL entry point for every source that exposes it, including pointers handed
// out before this call. *next is valid for every source. Install time or main thread.
bool Interpose(const char* glName, void* hook, void** next);
void SetMode(Mode m);
Mode GetMode();
int Count();
const char* Name(int i);
Src Source(int i);
const volatile uint32_t* Counters();  // per thunk, since start (Count/Log)
struct Rec { uint32_t frame; uint16_t fn; uint8_t pass; uint8_t flags; uint32_t caller; uint32_t a[8]; uint64_t tsc; };
// Log mode: records since `from` (a ring position); returns the new position.
uint32_t ReadRing(uint32_t from, void (*sink)(const Rec&, void*), void* user);
uint32_t RingPos();
bool IsThunk(const void* p);
}
