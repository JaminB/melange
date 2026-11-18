#pragma once
// MiragePose: render-only worm pose overrides (wum.render.wormPose / wormCopy / wormBones, Melange 0.16.0).
// The drawn worm is changed at three points of the engine's frame (pose_logic.h has the maths): its mesh transform, its
// animation clip weights and its skinning palette. The simulation never sees any of it.
#include <cstdint>

#include "render/mirage/pose_logic.h"

namespace melange::mirage::pose {
// From MirageShaders (shaders.cpp): every Cg program created (stage 0 vertex, 1 fragment), every cgGLBindProgram.
void OnCreate(void* program, const char* entry, int stage);
void OnBind(void* program);

enum class Result { Ok, Busy, Unavailable, Invalid };
constexpr int kMaxFrames = 600;
// One owner (a mod) per slot; the pose lasts `frames` frames (default 3) unless set again. Main thread. `body` and
// `clips` may be null (no edit there).
Result Set(const void* owner, int slot, const logic::Spec& s, const logic::Body* body, const logic::Clips* clips,
           int frames);
bool Clear(const void* owner, int slot);         // slot -1: every slot of the owner; true if one was cleared
Result SetCopy(const void* owner, int slot, const logic::M4& head, int frames);
bool ClearCopy(const void* owner, int slot);     // slot -1: every copy of the owner
// The id of a clip of the slot's worm (XAnimClipLibrary, case-insensitive name): -1 when there is no such clip, -2
// when the worm's library is not known yet (it was not drawn lately) or clips are off. Main thread.
int ClipId(int slot, const char* name);
// The world positions of the 34 bone pivots as last drawn (posed) for the slot, the neck point (pose_logic.h kNeck on
// the head bone) and the drawn mesh origin (`feet`: the pivot of a body turn), while a pose (a pose with no edits only
// tracks) or a copy is live on it; false when not drawn in the last 2 frames.
bool Bones(int slot, float out[logic::kBones][3], float neck[3], float feet[3]);
// "ok" once a draw of a posed, copied or tracked worm was identified and handled; "untested" while skinned worms were
// drawn but no such draw was identified yet; else "no skinned worm drawn yet", "unsupported" (no palette route on this
// build) or "disabled".
const char* Status();
uint32_t IdentifiedDraws();  // the number of such draws this session
struct Caps {
    bool body = false;             // Body: the mesh transform (hats and the shadow follow)
    bool clips = false;            // Clips: held clips and pose sliders
    bool palette = false;          // Spec: root/group edits, hand targets, the stump, wormBones
    bool copy = false;             // wormCopy
    bool hideAttachments = false;  // hide.attachments (and the hat under a stump)
};
Caps Capabilities();
}  // namespace melange::mirage::pose
