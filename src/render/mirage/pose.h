#pragma once
// MiragePose: render-only worm pose overrides (wum.render.wormPose / wormCopy / wormBones, Melange 0.16.0).
// The skinned worm draws are patched per draw (pose_logic.h has the maths); the simulation never sees any of it.
#include "render/mirage/pose_logic.h"

namespace melange::mirage::pose {
// From MirageShaders (shaders.cpp): every Cg program created (stage 0 vertex, 1 fragment), every cgGLBindProgram.
void OnCreate(void* program, const char* entry, int stage);
void OnBind(void* program);

enum class Result { Ok, Busy, Unavailable, Invalid };
constexpr int kMaxFrames = 600;
// One owner (a mod) per slot; the pose lasts `frames` frames (default 3) unless set again. Main thread.
Result Set(const void* owner, int slot, const logic::Spec& s, int frames);
bool Clear(const void* owner, int slot);         // slot -1: every slot of the owner; true if one was cleared
Result SetCopy(const void* owner, int slot, const logic::M4& head, int frames);
bool ClearCopy(const void* owner, int slot);     // slot -1: every copy of the owner
// The world positions of the 34 bone pivots as last drawn (posed) for the slot; false when not drawn recently.
bool Bones(int slot, float out[logic::kBones][3]);
// "ok" when the skinning palette was found (a skinned worm program was bound), else why not.
const char* Status();
}  // namespace melange::mirage::pose
