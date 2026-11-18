// Module "MiragePose": wum.render.wormPose / wormCopy / wormBones. Notes: docs/gravespade-re-worm.md (the GPU route),
// docs/gravespade-re-pose-cpu-skin.md, gravespade-re-pose-anim.md and gravespade-re-pose-vertex.md (the CPU route).
//
// THE CPU ROUTE (build #1077). XOglDrawAction (vtable 0x8b319c) overrides the vertex program name builder (slot 60,
// 0x790880) to return an empty name for every index with bit 8 ("Skinned"), so no skinned Cg program is ever created
// and worms are blended on the CPU at draw time. Install sees that override and puts three render-only layers in place,
// each at its own point of the engine's frame:
//
//   A. Body: a post-hook on WXWormGraphicEntity's update (slot 6, 0x5a4740, __stdcall(gfx, ms)). The vanilla update
//      ends by placing the mesh (0x5a2420: SetPosition(container.pos + (0,3,0)) and SetRotation({+0x158, +0x150,
//      +0x15c}) with force 0). For a posed slot this calls the worm XMeshInstance's (gfx+0x24) SetRotation (vtable
//      +0x54) and SetPosition (+0x50) again with force 1, so the drawn worm lies down or is carried onto another; the
//      hat, glasses, locators and the shadow follow. The next vanilla update writes its own transform back by itself.
//      It also records, per slot (gfx+0x64), the mesh, its XAnimInstance (mesh+0xc4) and the drawn origin, and hides
//      the head accessories (gfx+0x28 hat, +0x2c, +0x30; SetVisible, vtable +0x5c) for hide.attachments or a stump,
//      putting them back the way the engine's own mask counters say (+0x54 hat, +0x58 the others; 0x5a17d0).
//   B. Clips: a wrap of XAnimInstance::Evaluate (slot 9, 0x7ad0ef, __stdcall(anim)). For a posed slot's instance the
//      {weight, time, rate} table (+0x54..+0x58, one entry per clip id) gets the held clips and pose sliders, the
//      active-set flag +0xa4 is raised, Evaluate runs, and the table is put back. The scheduler, its anim events and
//      the game's clip choices never see it.
//   C. Palette: an inline hook on XOglDrawAction's XSkinShape method (0x790cd0, cdecl(iface, node)). A shape's bones
//      are node+0x48 (count +0x18, XBone* array +0x20); bone+0x70 is the CPU equivalent of BlendMat (inverse bind x
//      joint world, written by the cull pass 0x6ebede every frame), which 0x79a930 / 0x79ac70 blend positions and
//      normals with. For an identified posed worm the 34 matrices are replaced by the pose_logic.h Compose palette for
//      the call (and by ComposeCopy for a second call: the severed head), then put back. A and B already moved the
//      joints, so C's world-space edits stack on top of them.
//
// Which worm a skinned shape is (C): pose_logic.h SkinKeys, keyed by the shape's first XBone (per worm instance),
// learned on unambiguous draws against the drawn origins A records (so a worm drawn lying or mounted is still found).
// Nothing here writes sim state: the container, the logic entity, physics and the anim-event queue are never touched,
// and every engine write is to the drawn mesh or restored before the call returns.
//
// THE GPU ROUTE (a build that skins on the GPU; kept, not live on #1077). Worms are GPU-skinned by the Cg programs
// FFVertex*Skinned (CG/FixedFunction.cg): `float4x4 BlendMat[35] : register(c8)`, flushed by cgGL into the bound ARB
// vertex program's local parameters, four registers per bone. For a draw of a skinned program while a pose or copy is
// live, this module reads the palette back (glGetProgramLocalParameterfvARB), identifies the worm (pose_logic.h
// Identify), uploads the posed palette through the GL hub chain, draws, optionally draws the severed-head copy, and
// uploads the game's palette back. A draw is trusted only while the Cg program's ARB id is still bound and
// GL_VERTEX_PROGRAM_ARB is enabled. Rigid head attachments are separate unskinned draws there: with hide.attachments a
// draw whose `model` origin lies within 9 units of the posed worm's unposed head bones is skipped or moved with the
// head. Layers A and B install on any #1077 binary, so body and clips work on both routes.
#include <windows.h>
#include <GL/gl.h>
#include <safetyhook.hpp>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/gamestate.h"
#include "melange/sim.h"
#include "melange/testcmd.h"
#include "render/mirage/hub.h"
#include "render/mirage/pose.h"
#include "render/mirage/shaders_internal.h"

namespace melange::mirage::pose {
namespace {
namespace lg = logic;
constexpr GLenum kVertexProgramArb = 0x8620;
constexpr int kRegs = lg::kBones * 4;

using DrawElements = void(WINAPI*)(GLenum, GLsizei, GLenum, const void*);
using DrawArrays = void(WINAPI*)(GLenum, GLint, GLsizei);
using DrawRange = void(WINAPI*)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*);
using Ls4fv = void(WINAPI*)(GLenum, GLuint, GLsizei, const GLfloat*);
using L4fv = void(WINAPI*)(GLenum, GLuint, const GLfloat*);
using GetL = void(WINAPI*)(GLenum, GLuint, GLfloat*);
using BindProgram = void(WINAPI*)(GLenum, GLuint);
using Cap = void(WINAPI*)(GLenum);
DrawElements n_drawElements;
DrawArrays n_drawArrays;
DrawRange n_drawRange;
Ls4fv n_ls4fv;
L4fv n_l4fv;
BindProgram n_bindProgram;
Cap n_enable, n_disable;
GetL g_getLocal = nullptr;
bool g_getTried = false;

bool g_installed = false, g_log = false;
bool g_cpuSkinned = false;  // the engine's GL draw action never creates a skinned program (see the top)
// The CPU route's layers (A, B, C), each live only when its prologue matched.
SafetyHookInline g_hGfx, g_hEval, g_hSkin;
bool g_hasBody = false, g_hasClips = false, g_hasPalette = false;
bool g_sawCpuSkin = false;  // a 34-bone XSkinShape went through C

// The ARB vertex program state as the game (and cgGL) set it. Each check is used only once its call was seen, so a
// driver path that bypasses the hub degrades to the bind-only rule instead of turning poses off.
GLuint g_arbBound = 0;
bool g_vpEnabled = false, g_sawBind = false, g_sawEnable = false;

struct Prog {
    bool skinned = false;
    int stage = 0;
    bool resolved = false;
    int base = 8;        // skinned: BlendMat's first local register
    int modelBase = -1;  // unskinned: the `model` matrix's first local register, -1 when it has none
    GLuint arbId = 0;    // the ARB program cgGL bound for it
};
std::unordered_map<void*, Prog> g_progs;
Prog* g_vp = nullptr;      // the Cg vertex program last bound
bool g_sawSkinned = false;
uint32_t g_identified = 0;  // skinned draws of a posed / copied / tracked worm that were identified and handled

struct Slot {
    const void* owner = nullptr;
    uint64_t until = 0;
    lg::Spec spec;
    lg::Body body;
    lg::Clips clips;
    const void* copyOwner = nullptr;
    uint64_t copyUntil = 0;
    lg::M4 head = lg::Identity();
    float bones[lg::kBones][3] = {};
    float neck[3] = {};
    float feet[3] = {};
    uint64_t bonesFrame = 0;
    // Head attachments (GPU route): the unposed head bone pivots of the last draw and the head group's pose delta.
    float headPts[lg::kHeadBones][3] = {};
    uint64_t headFrame = 0;
    lg::M4 headDelta = lg::Identity();
};
Slot g_slots[16];
int g_live = 0;        // slots with a live pose or copy (recounted every frame)
int g_attachLive = 0;  // live poses with an attachment rule
int g_clipLive = 0;    // live poses with clip edits
uint64_t g_countFrame = ~0ull;
lg::Centre g_centre;

// What layer A saw of each slot's worm this frame.
struct Rec {
    uintptr_t mesh = 0, anim = 0;
    uint64_t frame = 0;
    float yaw = 0;           // the drawn yaw (gfx+0x150, radians)
    float vanilla[3] = {};   // the origin the vanilla update wrote
    float drawn[3] = {};     // the origin drawn (after a body move)
    bool hidden = false;     // the accessories are hidden by us
};
Rec g_rec[16];
lg::SkinKeys g_keys;

struct Worms {
    uint64_t frame = ~0ull;
    int n = 0;
    float feet[16][3];
    float centre[16][3];  // CPU route: the expected main pivot (the drawn origin when A saw the worm this frame)
    int slot[16];
} g_worms;

uint64_t Now() { return events::FrameCount(); }
bool PoseOn(const Slot& s) { return s.owner && Now() <= s.until; }
bool CopyOn(const Slot& s) { return s.copyOwner && Now() <= s.copyUntil; }
bool RecFresh(const Rec& r) { return r.mesh && r.frame + 2 >= Now(); }

void Recount() {
    g_live = g_attachLive = g_clipLive = 0;
    const bool match = sim::InMatch();
    for (Slot& s : g_slots) {
        if (s.owner && (!match || Now() > s.until)) s.owner = nullptr;
        if (s.copyOwner && (!match || Now() > s.copyUntil)) s.copyOwner = nullptr;
        g_live += (s.owner || s.copyOwner) ? 1 : 0;
        g_attachLive += s.owner && s.spec.attach != lg::Attach::Keep ? 1 : 0;
        g_clipLive += s.owner && s.clips.On() ? 1 : 0;
    }
}

// Once per frame while anything is live: expired poses go, and every pose goes at match end, so a finished pose
// stops the per-draw work.
void FrameTick() {
    const uint64_t f = Now();
    if (f == g_countFrame) return;
    g_countFrame = f;
    if (g_live) Recount();
}

void RefreshWorms() {
    const uint64_t f = Now();
    if (g_worms.frame == f) return;
    g_worms.frame = f;
    g_worms.n = 0;
    gamestate::Snapshot snap;
    if (!gamestate::Read(&snap)) return;
    for (int i = 0; i < snap.wormCount && g_worms.n < 16; ++i) {
        const gamestate::Worm& w = snap.worms[i];
        if (!w.active) continue;
        const int k = g_worms.n++;
        g_worms.feet[k][0] = w.pos.x;
        g_worms.feet[k][1] = w.pos.y;
        g_worms.feet[k][2] = w.pos.z;
        g_worms.slot[k] = w.slot;
        // The CPU route: where the main pivot is expected. A's drawn origin follows a body move; without it the feet.
        const Rec* r = (w.slot >= 0 && w.slot < 16 && RecFresh(g_rec[w.slot])) ? &g_rec[w.slot] : nullptr;
        if (r) {
            g_worms.centre[k][0] = r->drawn[0];
            g_worms.centre[k][1] = r->drawn[1] - lg::kDrawLift + g_centre.y;
            g_worms.centre[k][2] = r->drawn[2];
        } else {
            g_worms.centre[k][0] = w.pos.x;
            g_worms.centre[k][1] = w.pos.y + g_centre.y;
            g_worms.centre[k][2] = w.pos.z;
        }
    }
}

// ---------------------------------------------------------------- the CPU route
uintptr_t VT(uintptr_t obj, int off) { return *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(obj) + off); }
using SetV = int(__stdcall*)(uintptr_t, float*, int);
using SetVis = int(__stdcall*)(uintptr_t, int);
constexpr int kAccessory[3] = {0x28, 0x2c, 0x30};

void Accessories(uintptr_t gfx, bool hide) {
    for (int k = 0; k < 3; ++k) {
        const uintptr_t o = *reinterpret_cast<uintptr_t*>(gfx + kAccessory[k]);
        if (!o) continue;
        // Shown again the way the engine's mask counters say: the hat by m_nHatMask (+0x54), the others by +0x58.
        const int mask = *reinterpret_cast<int*>(gfx + (k == 0 ? 0x54 : 0x58));
        reinterpret_cast<SetVis>(VT(o, 0x5c))(o, hide ? 0 : (mask == 0 ? 1 : 0));
    }
}

void GfxPost(uintptr_t gfx) {
    const unsigned slot = *reinterpret_cast<uint8_t*>(gfx + 0x64);
    if (slot >= 16) return;
    const uintptr_t mesh = *reinterpret_cast<uintptr_t*>(gfx + 0x24);
    if (!mesh) return;
    Rec& r = g_rec[slot];
    if (r.mesh != mesh) {
        r = Rec{};
        r.mesh = mesh;
        g_keys.Forget(static_cast<int>(slot));
    }
    r.anim = *reinterpret_cast<uintptr_t*>(mesh + 0xc4);
    r.frame = Now();
    r.yaw = *reinterpret_cast<float*>(gfx + 0x150);
    std::memcpy(r.vanilla, reinterpret_cast<void*>(mesh + 0x10), sizeof r.vanilla);
    FrameTick();
    const Slot& s = g_slots[slot];
    const bool pose = g_live && PoseOn(s);
    if (pose && s.body.on) {
        if (s.body.turn) {
            float e[3];
            lg::BodyEuler(s.body, r.yaw, e);
            reinterpret_cast<SetV>(VT(mesh, 0x54))(mesh, e, 1);
        }
        float o[3];
        lg::BodyOrigin(s.body, r.vanilla, o);
        if (o[0] != r.vanilla[0] || o[1] != r.vanilla[1] || o[2] != r.vanilla[2])
            reinterpret_cast<SetV>(VT(mesh, 0x50))(mesh, o, 1);
    }
    std::memcpy(r.drawn, reinterpret_cast<void*>(mesh + 0x10), sizeof r.drawn);
    const bool hide = pose && (s.spec.stump || s.spec.attach == lg::Attach::Hide);
    if (hide) {
        Accessories(gfx, true);
        r.hidden = true;
    } else if (r.hidden) {
        Accessories(gfx, false);
        r.hidden = false;
    }
}

int __stdcall HkGfx(uintptr_t gfx, uint32_t ms) {
    const int ret = g_hGfx.stdcall<int>(gfx, ms);
    __try {
        GfxPost(gfx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ret;
}

constexpr int kMaxEntries = 1024;  // the worm library has 329 clips
float g_saved[kMaxEntries * 3];

int __stdcall HkEval(uintptr_t anim) {
    if (!g_clipLive || !anim) return g_hEval.stdcall<int>(anim);
    int slot = -1;
    for (int i = 0; i < 16; ++i)
        if (g_rec[i].anim == anim && RecFresh(g_rec[i]) && PoseOn(g_slots[i]) && g_slots[i].clips.On()) slot = i;
    if (slot < 0) return g_hEval.stdcall<int>(anim);
    float* b = *reinterpret_cast<float**>(anim + 0x54);
    float* e = *reinterpret_cast<float**>(anim + 0x58);
    const ptrdiff_t n = (b && e > b) ? (e - b) / 3 : 0;
    if (n <= 0 || n > kMaxEntries) return g_hEval.stdcall<int>(anim);
    std::memcpy(g_saved, b, static_cast<size_t>(n) * 3 * sizeof(float));
    lg::ApplyClips(g_slots[slot].clips, b, static_cast<int>(n));
    *reinterpret_cast<uint8_t*>(anim + 0xa4) = 1;  // weights crossed zero: rebuild the active channel set
    const int ret = g_hEval.stdcall<int>(anim);
    std::memcpy(b, g_saved, static_cast<size_t>(n) * 3 * sizeof(float));
    *reinterpret_cast<uint8_t*>(anim + 0xa4) = 1;
    return ret;
}

void PutPalette(const uintptr_t* bone, const float* v) {
    for (int b = 0; b < lg::kBones; ++b) std::memcpy(reinterpret_cast<void*>(bone[b] + 0x70), v + b * 16, 64);
}

int __cdecl HkSkinShape(uintptr_t iface, uintptr_t node) {
    FrameTick();
    if (!g_live || !node) return g_hSkin.ccall<int>(iface, node);
    const uintptr_t cont = *reinterpret_cast<uintptr_t*>(node + 0x48);
    if (!cont || *reinterpret_cast<uint32_t*>(cont + 0x18) != static_cast<uint32_t>(lg::kBones))
        return g_hSkin.ccall<int>(iface, node);
    const uintptr_t* bone = reinterpret_cast<const uintptr_t*>(cont + 0x20);
    for (int b = 0; b < lg::kBones; ++b)
        if (!bone[b]) return g_hSkin.ccall<int>(iface, node);
    g_sawCpuSkin = true;
    float pal[lg::kFloats], out[lg::kFloats];
    for (int b = 0; b < lg::kBones; ++b) std::memcpy(pal + b * 16, reinterpret_cast<const void*>(bone[b] + 0x70), 64);
    RefreshWorms();
    float main0[3];
    lg::PivotWorld(pal, 0, main0);
    const int i = g_keys.Resolve(static_cast<unsigned>(bone[0]), main0, g_worms.centre, g_worms.slot, g_worms.n, Now());
    if (i < 0) return g_hSkin.ccall<int>(iface, node);
    const int slot = g_worms.slot[i];
    if (slot < 0 || slot >= 16) return g_hSkin.ccall<int>(iface, node);
    Slot& s = g_slots[slot];
    const bool pose = PoseOn(s), copy = CopyOn(s);
    if (!pose && !copy) return g_hSkin.ccall<int>(iface, node);
    ++g_identified;
    int ret;
    const float* drawnPal = pal;
    if (pose && !s.spec.Empty()) {
        lg::Compose(pal, s.spec, out, &s.headDelta);
        PutPalette(bone, out);
        ret = g_hSkin.ccall<int>(iface, node);
        drawnPal = out;
    } else {
        s.headDelta = lg::Identity();
        ret = g_hSkin.ccall<int>(iface, node);
    }
    for (int b = 0; b < lg::kBones; ++b) lg::PivotWorld(drawnPal, b, s.bones[b]);
    float up[3];
    lg::NeckFrame(pal, s.headDelta, s.neck, up);
    const Rec& r = g_rec[slot];
    if (RecFresh(r)) {
        std::memcpy(s.feet, r.drawn, sizeof s.feet);
    } else {
        s.feet[0] = main0[0], s.feet[1] = main0[1] - g_centre.y + lg::kDrawLift, s.feet[2] = main0[2];
    }
    s.bonesFrame = Now();
    if (copy) {
        lg::ComposeCopy(pal, s.head, out);
        PutPalette(bone, out);
        g_hSkin.ccall<int>(iface, node);
    }
    if (drawnPal != pal || copy) PutPalette(bone, pal);
    return ret;
}

// The clip library of a slot's worm (mesh+0xc0, XAnimClipLibrary: vtable +0x18 -> {.., u16 count at +4}, +0x1c(id) ->
// {char* name, float length}), read into a name map once per library.
using Fn0 = uintptr_t(__stdcall*)(uintptr_t);
using Fn1 = uintptr_t(__stdcall*)(uintptr_t, int);
uintptr_t g_clipLib = 0;
std::unordered_map<std::string, int> g_clipIds;
const char* g_rawNames[kMaxEntries];

int ReadLibrary(uintptr_t lib) {
    int n = 0;
    __try {
        const uintptr_t hdr = reinterpret_cast<Fn0>(VT(lib, 0x18))(lib);
        n = hdr ? *reinterpret_cast<uint16_t*>(hdr + 4) : 0;
        if (n > kMaxEntries) n = kMaxEntries;
        for (int i = 0; i < n; ++i) {
            const uintptr_t rec = reinterpret_cast<Fn1>(VT(lib, 0x1c))(lib, i);
            g_rawNames[i] = rec ? *reinterpret_cast<const char**>(rec) : nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

std::string Lower(const char* s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

void Probe(int slot) {
    // pose.probe: the live state of one slot, for the logs
    const Rec& r = g_rec[slot];
    const Slot& s = g_slots[slot];
    LOG_INFO("[pose] slot %d mesh %08x anim %08x fresh %d drawn (%.1f %.1f %.1f) vanilla (%.1f %.1f %.1f) yaw %.3f pose %d "
             "body %d clips %d/%d bones %d centre %.2f (%d)",
             slot, static_cast<unsigned>(r.mesh), static_cast<unsigned>(r.anim), RecFresh(r), r.drawn[0], r.drawn[1],
             r.drawn[2], r.vanilla[0], r.vanilla[1], r.vanilla[2], r.yaw, PoseOn(s), s.body.on, s.clips.n, s.clips.sn,
             s.bonesFrame + 2 >= Now(), g_centre.y, g_centre.samples);
}

// ---------------------------------------------------------------- the GPU route
void Resolve(void* program, Prog& p) {
    if (p.resolved) return;
    p.resolved = true;
    const shadersrc::Cg& cg = shaders::CgApi();
    if (cg.ok && cg.GetNamedParameter && cg.GetParameterResourceIndex) {
        if (p.skinned) {
            void* param = cg.GetNamedParameter(program, "BlendMat[0]");
            if (!param) param = cg.GetNamedParameter(program, "BlendMat");
            if (param) {
                const unsigned long idx = cg.GetParameterResourceIndex(param);
                if (idx < 256) p.base = static_cast<int>(idx);
            }
        } else if (void* param = cg.GetNamedParameter(program, "model")) {
            const unsigned long idx = cg.GetParameterResourceIndex(param);
            if (idx < 256) p.modelBase = static_cast<int>(idx);
        }
        cg.Drain();
    }
    if (g_log && p.skinned) LOG_INFO("[pose] skinned program %p: BlendMat at program.local[%d]", program, p.base);
}

bool ReadRegs(int base, float* out, int first, int count) {
    if (!g_getTried) {
        g_getTried = true;
        PROC f = wglGetProcAddress("glGetProgramLocalParameterfvARB");
        const auto v = reinterpret_cast<intptr_t>(f);
        g_getLocal = (f && v != 1 && v != 2 && v != 3 && v != -1) ? reinterpret_cast<GetL>(f) : nullptr;
        if (!g_getLocal) LOG_WARN("[pose] glGetProgramLocalParameterfvARB is missing: worm poses are off");
    }
    if (!g_getLocal) return false;
    for (int k = first; k < first + count; ++k) g_getLocal(kVertexProgramArb, static_cast<GLuint>(base + k), out + k * 4);
    return true;
}

void Upload(int base, const float* v, int regs = kRegs) {
    if (n_ls4fv) {
        n_ls4fv(kVertexProgramArb, static_cast<GLuint>(base), regs, v);
        return;
    }
    for (int k = 0; k < regs; ++k) n_l4fv(kVertexProgramArb, static_cast<GLuint>(base + k), v + k * 4);
}

// The draw really runs the Cg vertex program last bound (see the top).
bool VpCurrent() {
    if (!g_vp) return false;
    if (g_sawEnable && !g_vpEnabled) return false;
    if (g_sawBind && (!g_vp->arbId || g_arbBound != g_vp->arbId)) return false;
    return true;
}

template <class Draw>
void Skinned(Draw draw) {
    float pal[lg::kFloats], out[lg::kFloats];
    // The main bone (4 registers) says which worm this is; the rest of the palette is read only for a posed one.
    if (!ReadRegs(g_vp->base, pal, 0, 4)) return draw();
    RefreshWorms();
    float main0[3];
    lg::PivotWorld(pal, 0, main0);
    g_centre.Sample(main0, g_worms.feet, g_worms.n);
    const int i = lg::Identify(pal, g_worms.feet, g_worms.n, 26.f, g_centre.y);
    if (i < 0) return draw();
    const int slot = g_worms.slot[i];
    Slot& s = g_slots[slot];
    const bool pose = PoseOn(s), copy = CopyOn(s);
    if (!pose && !copy) return draw();
    ReadRegs(g_vp->base, pal, 4, kRegs - 4);
    ++g_identified;
    for (int b = 0; b < lg::kHeadBones; ++b) lg::PivotWorld(pal, lg::kHeadFirst + b, s.headPts[b]);
    s.headFrame = Now();
    bool uploaded = false;
    const float* drawnPal = pal;
    if (pose && !s.spec.Empty()) {
        lg::Compose(pal, s.spec, out, &s.headDelta);
        Upload(g_vp->base, out);
        uploaded = true;
        draw();
        drawnPal = out;
    } else {
        s.headDelta = lg::Identity();
        draw();
    }
    for (int b = 0; b < lg::kBones; ++b) lg::PivotWorld(drawnPal, b, s.bones[b]);
    float up[3];
    lg::NeckFrame(pal, s.headDelta, s.neck, up);
    lg::Point(lg::BoneOf(drawnPal, 0), lg::kPivot[0], s.feet);
    s.bonesFrame = Now();
    if (copy) {
        lg::ComposeCopy(pal, s.head, out);
        Upload(g_vp->base, out);
        uploaded = true;
        draw();
    }
    if (uploaded) Upload(g_vp->base, pal);
}

// An unskinned draw while a pose with an attachment rule is live: is it a head attachment of that worm?
template <class Draw>
void Attachment(Draw draw) {
    float m[16];
    if (!ReadRegs(g_vp->modelBase, m, 0, 4)) return draw();
    const float t[3] = {m[12], m[13], m[14]};
    for (Slot& s : g_slots) {
        if (!PoseOn(s) || s.spec.attach == lg::Attach::Keep || !s.headFrame || Now() > s.headFrame + 2) continue;
        if (!lg::NearHead(t, s.headPts)) continue;
        if (s.spec.attach == lg::Attach::Hide || s.spec.stump) return;  // not drawn
        lg::M4 model;
        std::memcpy(model.m, m, sizeof m);
        const lg::M4 moved = lg::Mul(model, s.headDelta);
        Upload(g_vp->modelBase, moved.m, 4);
        draw();
        Upload(g_vp->modelBase, m, 4);
        return;
    }
    draw();
}

template <class Draw>
void Around(Draw draw) {
    FrameTick();
    if (!g_live || !VpCurrent()) return draw();
    if (g_vp->skinned) return Skinned(draw);
    if (g_attachLive && g_vp->modelBase >= 0) return Attachment(draw);
    draw();
}

void WINAPI HkDrawElements(GLenum m, GLsizei c, GLenum t, const void* i) {
    Around([&] { n_drawElements(m, c, t, i); });
}
void WINAPI HkDrawArrays(GLenum m, GLint f, GLsizei c) {
    Around([&] { n_drawArrays(m, f, c); });
}
void WINAPI HkDrawRange(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void* i) {
    Around([&] { n_drawRange(m, s, e, c, t, i); });
}
// Pass-through wrappers so our uploads enter the chain in front of MirageShaders' mirror.
void WINAPI HkLs4fv(GLenum t, GLuint i, GLsizei n, const GLfloat* v) { n_ls4fv(t, i, n, v); }
void WINAPI HkL4fv(GLenum t, GLuint i, const GLfloat* v) { n_l4fv(t, i, v); }
// The ARB vertex program state.
void WINAPI HkBindProgram(GLenum target, GLuint id) {
    n_bindProgram(target, id);
    if (target != kVertexProgramArb) return;
    g_arbBound = id;
    g_sawBind = true;
}
void WINAPI HkEnable(GLenum cap) {
    n_enable(cap);
    if (cap != kVertexProgramArb) return;
    g_vpEnabled = true;
    g_sawEnable = true;
}
void WINAPI HkDisable(GLenum cap) {
    n_disable(cap);
    if (cap != kVertexProgramArb) return;
    g_vpEnabled = false;
    g_sawEnable = true;
}

template <class T>
bool Chain(const char* name, T hook, T* next) {
    return hub::Interpose(name, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(next));
}

bool ValidSlot(int slot) { return slot >= 0 && slot < 16; }
int Frames(int frames) { return frames <= 0 ? 3 : (frames > kMaxFrames ? kMaxFrames : frames); }

bool VerbTest(std::string_view args, void*) {
    // pose.test <slot> prone|stump|mount|copy|lie|ride <other>|clip <name> <t> <w>|probe|off : a 250-frame pose (about
    // 4 s at 60 fps) for screenshots, owned by the console.
    static const int kOwner = 0;
    int slot = -1;
    char what[16] = "", arg[48] = "";
    float f1 = 0, f2 = 1;
    std::string a(args);
    if (sscanf_s(a.c_str(), "%d %15s %47s %f %f", &slot, what, static_cast<unsigned>(sizeof what), arg,
                 static_cast<unsigned>(sizeof arg), &f1, &f2) < 1 ||
        !ValidSlot(slot)) {
        LOG_INFO("[pose] pose.test <slot> prone|stump|mount|copy|lie|ride <slot>|clip <name> <t> <w>|probe|off (status: %s)",
                 Status());
        return false;
    }
    lg::Spec s;
    lg::Body body;
    lg::Clips clips;
    if (!strcmp(what, "probe")) {
        Probe(slot);
        return true;
    }
    if (!strcmp(what, "off")) {
        Clear(&kOwner, slot);
        ClearCopy(&kOwner, slot);
        return true;
    }
    if (!strcmp(what, "copy")) {
        float pos[3] = {30, 0, 0};
        lg::M4 h = lg::Mul(lg::Rotate(0, 0, 90), lg::Translate(pos));
        RefreshWorms();
        for (int i = 0; i < g_worms.n; ++i)
            if (g_worms.slot[i] == slot) {
                const float p[3] = {g_worms.feet[i][0] + 30, g_worms.feet[i][1] + 5, g_worms.feet[i][2]};
                h = lg::Mul(lg::Rotate(0, 0, 90), lg::Translate(p));
            }
        SetCopy(&kOwner, slot, h, 250);
        s.stump = true;
        s.attach = lg::Attach::Hide;
    } else if (!strcmp(what, "prone")) {
        s.root.on = true;
        s.root.rot[0] = -90;
        s.attach = lg::Attach::Follow;
    } else if (!strcmp(what, "stump")) {
        s.stump = true;
        s.attach = lg::Attach::Hide;
    } else if (!strcmp(what, "mount")) {
        s.root.on = true;
        s.root.rot[0] = 35;
        s.root.move[1] = 6;
        s.group[lg::kRightHand].on = true;
        s.group[lg::kRightHand].rot[0] = -60;
        s.group[lg::kLeftHand].on = true;
        s.group[lg::kLeftHand].rot[0] = -60;
        s.attach = lg::Attach::Follow;
    } else if (!strcmp(what, "lie")) {
        // layer A: on its back about the feet, raised onto the ground
        body.on = body.turn = true;
        body.pitch = -90;
        body.move[1] = 4;
    } else if (!strcmp(what, "ride")) {
        // layer A: carried onto another worm's belly (that worm lying with "lie"), facing its head, leaning in
        const int other = atoi(arg);
        if (!ValidSlot(other) || !RecFresh(g_rec[other]) || !RecFresh(g_rec[slot])) return false;
        const Rec& v = g_rec[other];
        const float fx = std::sin(v.yaw), fz = std::cos(v.yaw);
        body.on = body.turn = true;
        body.absHeading = true;
        body.yaw = v.yaw * 57.2957795f + 180.f;
        body.pitch = 30;
        body.absAt = true;
        body.move[0] = v.vanilla[0] - fx * 8;
        body.move[1] = v.vanilla[1] - lg::kDrawLift + 6.5f;
        body.move[2] = v.vanilla[2] - fz * 8;
    } else if (!strcmp(what, "clip")) {
        const int id = ClipId(slot, arg);
        if (id < 0) {
            LOG_INFO("[pose] pose.test: no clip %s (%d)", arg, id);
            return false;
        }
        clips.n = 1;
        clips.id[0] = static_cast<short>(id);
        clips.time[0] = lg::ClampTime(f1);
        clips.weight[0] = lg::ClampWeight(f2);
    }
    const Result r = Set(&kOwner, slot, s, &body, &clips, 250);
    LOG_INFO("[pose] pose.test %d %s: %s (status: %s, centre %.2f from %d sample(s))", slot, what,
             r == Result::Ok ? "ok" : "refused", Status(), g_centre.y, g_centre.samples);
    return r == Result::Ok;
}
}  // namespace

void OnCreate(void* program, const char* entry, int stage) {
    if (!g_installed || g_cpuSkinned || !program) return;
    Prog p;
    p.stage = stage;
    p.skinned = stage == 0 && entry && strstr(entry, "Skinned") != nullptr;
    g_progs[program] = p;
    if (p.skinned && g_log) LOG_INFO("[pose] skinned vertex program %s created", entry);
}

void OnBind(void* program) {
    if (!g_installed || g_cpuSkinned || !program) return;
    auto it = g_progs.find(program);
    if (it == g_progs.end()) return;
    if (it->second.stage != 0) return;
    g_vp = &it->second;
    g_vp->arbId = g_arbBound;  // cgGL's glBindProgramARB went through HkBindProgram just now (or was already bound)
    Resolve(program, *g_vp);
    if (g_vp->skinned) g_sawSkinned = true;
}

Result Set(const void* owner, int slot, const lg::Spec& s, const lg::Body* body, const lg::Clips* clips, int frames) {
    if (!g_installed || !sim::InMatch()) return Result::Unavailable;
    if (!ValidSlot(slot)) return Result::Invalid;
    Slot& d = g_slots[slot];
    if (d.owner && d.owner != owner && Now() <= d.until) return Result::Busy;
    d.owner = owner;
    d.spec = s;
    d.body = (body && g_hasBody) ? *body : lg::Body{};
    d.clips = (clips && g_hasClips) ? *clips : lg::Clips{};
    d.until = Now() + static_cast<uint64_t>(Frames(frames));
    Recount();
    return Result::Ok;
}

bool Clear(const void* owner, int slot) {
    bool any = false;
    for (int i = 0; i < 16; ++i)
        if ((slot < 0 || slot == i) && g_slots[i].owner && (!owner || g_slots[i].owner == owner)) {
            g_slots[i].owner = nullptr;
            any = true;
        }
    Recount();
    return any;
}

Result SetCopy(const void* owner, int slot, const lg::M4& head, int frames) {
    if (!g_installed || !sim::InMatch() || (g_cpuSkinned && !g_hasPalette)) return Result::Unavailable;
    if (!ValidSlot(slot)) return Result::Invalid;
    Slot& d = g_slots[slot];
    if (d.copyOwner && d.copyOwner != owner && Now() <= d.copyUntil) return Result::Busy;
    d.copyOwner = owner;
    d.head = head;
    d.copyUntil = Now() + static_cast<uint64_t>(Frames(frames));
    Recount();
    return Result::Ok;
}

bool ClearCopy(const void* owner, int slot) {
    bool any = false;
    for (int i = 0; i < 16; ++i)
        if ((slot < 0 || slot == i) && g_slots[i].copyOwner && (!owner || g_slots[i].copyOwner == owner)) {
            g_slots[i].copyOwner = nullptr;
            any = true;
        }
    Recount();
    return any;
}

int ClipId(int slot, const char* name) {
    if (!g_hasClips || !ValidSlot(slot) || !name || !RecFresh(g_rec[slot])) return -2;
    const uintptr_t lib = *reinterpret_cast<uintptr_t*>(g_rec[slot].mesh + 0xc0);
    if (!lib) return -2;
    if (lib != g_clipLib) {
        const int n = ReadLibrary(lib);
        if (n <= 0) return -2;
        g_clipLib = lib;
        g_clipIds.clear();
        for (int i = 0; i < n; ++i)
            if (g_rawNames[i]) g_clipIds.emplace(Lower(g_rawNames[i]), i);
        if (g_log) LOG_INFO("[pose] clip library %08x: %d clips", static_cast<unsigned>(lib), n);
    }
    auto it = g_clipIds.find(Lower(name));
    return it == g_clipIds.end() ? -1 : it->second;
}

bool Bones(int slot, float out[lg::kBones][3], float neck[3], float feet[3]) {
    if (!ValidSlot(slot) || !g_slots[slot].bonesFrame || Now() > g_slots[slot].bonesFrame + 2) return false;
    const Slot& s = g_slots[slot];
    memcpy(out, s.bones, sizeof s.bones);
    if (neck) memcpy(neck, s.neck, sizeof s.neck);
    if (feet) memcpy(feet, s.feet, sizeof s.feet);
    return true;
}

const char* Status() {
    if (g_cpuSkinned) {
        if (!g_hasPalette) return g_installed ? "unsupported" : "disabled";
        if (!g_sawCpuSkin) return "no skinned worm drawn yet";
        return g_identified ? "ok" : "untested";
    }
    if (!g_installed) return "disabled";
    if (g_getTried && !g_getLocal) return "unsupported";
    if (!g_sawSkinned) return "no skinned worm drawn yet";
    if (!g_identified) return "untested";
    return "ok";
}

uint32_t IdentifiedDraws() { return g_identified; }

Caps Capabilities() {
    Caps c;
    if (!g_installed) return c;
    c.body = g_hasBody;
    c.clips = g_hasClips;
    c.palette = c.copy = g_cpuSkinned ? g_hasPalette : true;
    c.hideAttachments = g_cpuSkinned ? g_hasBody : true;
    return c;
}

namespace {
class MiragePose final : public melange::Module {
public:
    const char* Name() const override { return "MiragePose"; }
    const char* Description() const override { return "render-only worm poses for mods (wum.render.wormPose)"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 44; }  // after MirageShaders (43): our GL wrappers sit in front of its mirror
    bool Install() override {
        g_log = Bool("Log", false);
        // Layers A and B (any #1077 binary): the worm graphic update and XAnimInstance::Evaluate.
        if (melange::mem::Expect(0x5a4740, {0x83, 0xec, 0x0c, 0x56, 0x8b, 0x74, 0x24, 0x14})) {
            g_hGfx = safetyhook::create_inline(0x5a4740, &HkGfx);
            g_hasBody = static_cast<bool>(g_hGfx);
        }
        if (g_hasBody && melange::mem::Expect(0x7ad0ef, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x2c, 0x56, 0x8b, 0x75, 0x08})) {
            g_hEval = safetyhook::create_inline(0x7ad0ef, &HkEval);
            g_hasClips = static_cast<bool>(g_hEval);
        }
        // XOglDrawAction's slot 60 is 0x790880, whose first test is `test al, 8; je` (the Skinned bit -> empty name).
        g_cpuSkinned = melange::mem::Expect(0x8b319c + 60 * 4, {0x80, 0x08, 0x79, 0x00}) &&
                       melange::mem::Expect(0x790880, {0x8b, 0x44, 0x24, 0x0c, 0x56, 0x8b, 0x74, 0x24, 0x0c, 0xa8, 0x08, 0x74});
        if (g_cpuSkinned) {
            // Layer C: XOglDrawAction's XSkinShape method.
            if (melange::mem::Expect(0x790cd0, {0x8b, 0x54, 0x24, 0x04, 0x53, 0x56, 0x57, 0x85, 0xd2})) {
                g_hSkin = safetyhook::create_inline(0x790cd0, &HkSkinShape);
                g_hasPalette = static_cast<bool>(g_hSkin);
            }
            g_centre.y = lg::kCpuCentreY;
            g_installed = g_hasBody || g_hasClips || g_hasPalette;
            if (g_installed) melange::testcmd::Register("pose.test", &VerbTest);
            LOG_INFO("[pose] installed on the CPU-skinned route: body %s, clips %s, palette %s (render only)",
                     g_hasBody ? "on" : "off", g_hasClips ? "on" : "off", g_hasPalette ? "on" : "off");
            return g_installed;
        }
        if (!shaders::CgApi().ok || !shaders::RequireBindHook() || !hub::Require(Name())) {
            LOG_WARN("[pose] the Cg layer or the GL hub is unavailable: wum.render.wormPose answers \"unavailable\"");
            return false;
        }
        const bool ok = Chain("glDrawElements", &HkDrawElements, &n_drawElements) &&
                        Chain("glDrawArrays", &HkDrawArrays, &n_drawArrays) &&
                        Chain("glDrawRangeElements", &HkDrawRange, &n_drawRange) &&
                        Chain("glProgramLocalParameter4fvARB", &HkL4fv, &n_l4fv);
        Chain("glProgramLocalParameters4fvEXT", &HkLs4fv, &n_ls4fv);  // optional: one call per palette when present
        // Optional: without them a draw is trusted to be the last bound Cg program (the bind-only rule).
        const bool state = Chain("glBindProgramARB", &HkBindProgram, &n_bindProgram) &&
                           Chain("glEnable", &HkEnable, &n_enable) && Chain("glDisable", &HkDisable, &n_disable);
        if (!state) LOG_WARN("[pose] the ARB program state is not tracked: poses trust the last Cg bind");
        if (!ok) {
            LOG_WARN("[pose] GL draw hooks incomplete: wum.render.wormPose answers \"unavailable\"");
            return false;
        }
        g_installed = true;
        melange::testcmd::Register("pose.test", &VerbTest);
        LOG_INFO("[pose] installed: worm poses patch the skinning palette per draw (render only)");
        return true;
    }
};
MELANGE_MODULE(MiragePose);
}  // namespace
}  // namespace melange::mirage::pose
