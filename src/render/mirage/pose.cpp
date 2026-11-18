// Module "MiragePose": wum.render.wormPose / wormCopy / wormBones. Notes: docs/gravespade-re-worm.md, route A.
//
// Worms are GPU-skinned by the Cg programs FFVertex*Skinned (CG/FixedFunction.cg): `float4x4 BlendMat[35] :
// register(c8)`, flushed by cgGL into the bound ARB vertex program's local parameters, four registers per bone. For
// a draw of a skinned program while a pose or copy is live, this module reads the 34-bone palette back
// (glGetProgramLocalParameterfvARB), finds which worm it is (pose_logic.h Identify: the main bone's pivot against
// each worm's feet), uploads the posed palette through the GL hub chain (so MirageShaders' GLSL mirror sees it when a
// GLSL replacement draws the worm), draws, optionally draws again with the severed-head palette, and uploads the
// game's palette back so cgGL's shadow state stays true. Draws only: the simulation, Wormsign and physics never see
// a pose, so this cannot desync. With no pose live every draw goes straight through.
//
// Not covered: rigid attachments (hats, moustaches, the engine-held weapon) are separate unskinned draws and keep
// the unposed position; give a posed tool an invisible held mesh and draw it with wum.draw.mesh.
#include <windows.h>
#include <GL/gl.h>

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/events.h"
#include "core/log.h"
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
DrawElements n_drawElements;
DrawArrays n_drawArrays;
DrawRange n_drawRange;
Ls4fv n_ls4fv;
L4fv n_l4fv;
GetL g_getLocal = nullptr;
bool g_getTried = false;

bool g_installed = false, g_log = false;

struct Prog {
    bool skinned = false;
    int stage = 0;
    bool resolved = false;
    int base = 8;
};
std::unordered_map<void*, Prog> g_progs;
Prog* g_vp = nullptr;      // the bound vertex program, when we know it
bool g_sawSkinned = false;

struct Slot {
    const void* owner = nullptr;
    uint64_t until = 0;
    lg::Spec spec;
    const void* copyOwner = nullptr;
    uint64_t copyUntil = 0;
    lg::M4 head = lg::Identity();
    float bones[lg::kBones][3] = {};
    uint64_t bonesFrame = 0;
};
Slot g_slots[16];
int g_live = 0;  // slots with a pose or copy owner (cleared lazily on expiry)

struct Worms {
    uint64_t frame = ~0ull;
    int n = 0;
    float feet[16][3];
    int slot[16];
} g_worms;

uint64_t Now() { return events::FrameCount(); }
bool PoseOn(const Slot& s) { return s.owner && Now() <= s.until; }
bool CopyOn(const Slot& s) { return s.copyOwner && Now() <= s.copyUntil; }

void Recount() {
    g_live = 0;
    for (Slot& s : g_slots) {
        if (s.owner && Now() > s.until) s.owner = nullptr;
        if (s.copyOwner && Now() > s.copyUntil) s.copyOwner = nullptr;
        g_live += (s.owner || s.copyOwner) ? 1 : 0;
    }
}

void RefreshWorms() {
    const uint64_t f = Now();
    if (g_worms.frame == f) return;
    g_worms.frame = f;
    g_worms.n = 0;
    gamestate::Snapshot snap;
    if (!gamestate::Read(&snap)) return;
    for (int i = 0; i < snap.wormCount; ++i) {
        const gamestate::Worm& w = snap.worms[i];
        if (!w.active) continue;
        g_worms.feet[g_worms.n][0] = w.pos.x;
        g_worms.feet[g_worms.n][1] = w.pos.y;
        g_worms.feet[g_worms.n][2] = w.pos.z;
        g_worms.slot[g_worms.n++] = w.slot;
    }
}

void Resolve(void* program, Prog& p) {
    if (p.resolved) return;
    p.resolved = true;
    const shadersrc::Cg& cg = shaders::CgApi();
    if (cg.ok && cg.GetNamedParameter && cg.GetParameterResourceIndex) {
        void* param = cg.GetNamedParameter(program, "BlendMat[0]");
        if (!param) param = cg.GetNamedParameter(program, "BlendMat");
        if (param) {
            const unsigned long idx = cg.GetParameterResourceIndex(param);
            if (idx < 256) p.base = static_cast<int>(idx);
        }
        cg.Drain();
    }
    if (g_log) LOG_INFO("[pose] skinned program %p: BlendMat at program.local[%d]", program, p.base);
}

bool ReadPalette(int base, float* out, int first = 0, int count = kRegs) {
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

void Upload(int base, const float* v) {
    if (n_ls4fv) {
        n_ls4fv(kVertexProgramArb, static_cast<GLuint>(base), kRegs, v);
        return;
    }
    for (int k = 0; k < kRegs; ++k) n_l4fv(kVertexProgramArb, static_cast<GLuint>(base + k), v + k * 4);
}

template <class Draw>
void Around(Draw draw) {
    if (!g_live || !g_vp || !g_vp->skinned) return draw();
    float pal[lg::kFloats], out[lg::kFloats];
    // The main bone (4 registers) says which worm this is; the rest of the palette is read only for a posed one.
    if (!ReadPalette(g_vp->base, pal, 0, 4)) return draw();
    RefreshWorms();
    const int i = lg::Identify(pal, g_worms.feet, g_worms.n, 26.f);
    if (i < 0) return draw();
    const int slot = g_worms.slot[i];
    Slot& s = g_slots[slot];
    const bool pose = PoseOn(s), copy = CopyOn(s);
    if (!pose && !copy) return draw();
    ReadPalette(g_vp->base, pal, 4, kRegs - 4);
    if (pose) {
        lg::Compose(pal, s.spec, out);
        Upload(g_vp->base, out);
        draw();
        for (int b = 0; b < lg::kBones; ++b) lg::PivotWorld(out, b, s.bones[b]);
    } else {
        draw();
        for (int b = 0; b < lg::kBones; ++b) lg::PivotWorld(pal, b, s.bones[b]);
    }
    s.bonesFrame = Now();
    if (copy) {
        lg::ComposeCopy(pal, s.head, out);
        Upload(g_vp->base, out);
        draw();
    }
    Upload(g_vp->base, pal);
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

template <class T>
bool Chain(const char* name, T hook, T* next) {
    return hub::Interpose(name, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(next));
}

bool ValidSlot(int slot) { return slot >= 0 && slot < 16; }
int Frames(int frames) { return frames <= 0 ? 3 : (frames > kMaxFrames ? kMaxFrames : frames); }

bool VerbTest(std::string_view args, void*) {
    // pose.test <slot> prone|stump|mount|copy|off : a 5 s pose for screenshots, owned by the console.
    static const int kOwner = 0;
    int slot = -1;
    char what[16] = "";
    std::string a(args);
    if (sscanf_s(a.c_str(), "%d %15s", &slot, what, static_cast<unsigned>(sizeof what)) < 1 || !ValidSlot(slot)) {
        LOG_INFO("[pose] pose.test <slot> prone|stump|mount|copy|off (status: %s)", Status());
        return false;
    }
    lg::Spec s;
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
    } else if (!strcmp(what, "prone")) {
        s.root.on = true;
        s.root.rot[0] = -90;
    } else if (!strcmp(what, "stump")) {
        s.stump = true;
    } else if (!strcmp(what, "mount")) {
        s.root.on = true;
        s.root.rot[0] = 35;
        s.root.move[1] = 6;
        s.group[lg::kRightHand].on = true;
        s.group[lg::kRightHand].rot[0] = -60;
        s.group[lg::kLeftHand].on = true;
        s.group[lg::kLeftHand].rot[0] = -60;
    }
    const Result r = Set(&kOwner, slot, s, 250);
    LOG_INFO("[pose] pose.test %d %s: %s (status: %s)", slot, what, r == Result::Ok ? "ok" : "refused", Status());
    return r == Result::Ok;
}
}  // namespace

void OnCreate(void* program, const char* entry, int stage) {
    if (!g_installed || !program) return;
    Prog p;
    p.stage = stage;
    p.skinned = stage == 0 && entry && strstr(entry, "Skinned") != nullptr;
    g_progs[program] = p;
    if (p.skinned && g_log) LOG_INFO("[pose] skinned vertex program %s created", entry);
}

void OnBind(void* program) {
    if (!g_installed || !program) return;
    auto it = g_progs.find(program);
    if (it == g_progs.end()) return;
    if (it->second.stage != 0) return;
    g_vp = &it->second;
    if (g_vp->skinned) {
        Resolve(program, *g_vp);
        g_sawSkinned = true;
    }
}

Result Set(const void* owner, int slot, const lg::Spec& s, int frames) {
    if (!g_installed || !sim::InMatch()) return Result::Unavailable;
    if (!ValidSlot(slot)) return Result::Invalid;
    Slot& d = g_slots[slot];
    if (d.owner && d.owner != owner && Now() <= d.until) return Result::Busy;
    d.owner = owner;
    d.spec = s;
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
    if (!g_installed || !sim::InMatch()) return Result::Unavailable;
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

bool Bones(int slot, float out[lg::kBones][3]) {
    if (!ValidSlot(slot) || !g_slots[slot].bonesFrame || Now() > g_slots[slot].bonesFrame + 2) return false;
    memcpy(out, g_slots[slot].bones, sizeof g_slots[slot].bones);
    return true;
}

const char* Status() {
    if (!g_installed) return "disabled";
    if (!g_sawSkinned) return "no skinned worm drawn yet";
    if (g_getTried && !g_getLocal) return "unsupported";
    return "ok";
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
        if (!shaders::CgApi().ok || !shaders::RequireBindHook() || !hub::Require(Name())) {
            LOG_WARN("[pose] the Cg layer or the GL hub is unavailable: wum.render.wormPose answers \"unavailable\"");
            return false;
        }
        const bool ok = Chain("glDrawElements", &HkDrawElements, &n_drawElements) &&
                        Chain("glDrawArrays", &HkDrawArrays, &n_drawArrays) &&
                        Chain("glDrawRangeElements", &HkDrawRange, &n_drawRange) &&
                        Chain("glProgramLocalParameter4fvARB", &HkL4fv, &n_l4fv);
        Chain("glProgramLocalParameters4fvEXT", &HkLs4fv, &n_ls4fv);  // optional: one call per palette when present
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
