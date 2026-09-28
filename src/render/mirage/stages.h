#pragma once
// Stage -> (bucket id, pre/post) table and the multiplexer behind render::AddStageCallback.
#include <cstdint>
#include <string>

#include "melange/render.h"

namespace melange::mirage::stages {
struct Slot { int bucket; bool post; };
Slot Default(render::Stage s);  // the frozen table
Slot Get(render::Stage s);      // after [Mirage] StageIds overrides
const char* Name(render::Stage s);

// Mirage module only.
bool Configure(const std::string& overrides);  // "World=pre50,Hud=post163"; false (and defaults) on a parse error
void Enable();
void OnFrame();  // main thread: installs wanted slots, re-asserts installed ones, restores emptied ones

// Components skip installing when the Mirage module is switched off in the ini.
bool CoreEnabled(const char* who);
// True once the Mirage scaffold actually enabled (engine::Check() passed in Mirage::Install()). Unlike
// CoreEnabled(), this also reflects an unrecognised exe build, so callers that would otherwise queue work forever
// waiting on stage callbacks that can never register (AddStageCallback returns 0 while this is false) can skip it.
bool Enabled();

struct Stats { uint64_t reasserts, mainPasses, otherPasses, faults, removed; };
Stats GetStats();
// Our scene-func objects, for the audit.
bool IsOwnObject(uintptr_t obj);
int Callbacks(render::Stage s);
}
