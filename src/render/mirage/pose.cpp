// Module "MiragePose": wum.render.wormPose / wormCopy / wormBones. Notes: docs/gravespade-re-worm.md, route A.
//
// Worms are GPU-skinned by the Cg programs FFVertex*Skinned (CG/FixedFunction.cg): `float4x4 BlendMat[35] :
// register(c8)`, flushed by cgGL into the bound ARB vertex program's local parameters, four registers per bone. For
// a draw of a skinned program while a pose or copy is live, this module reads the 34-bone palette back
// (glGetProgramLocalParameterfvARB), finds which worm it is (pose_logic.h Identify: the main bone's pivot against
// each worm's feet, with the centre height learned in game), uploads the posed palette through the GL hub chain (so
// MirageShaders' GLSL mirror sees it when a GLSL replacement draws the worm), draws, optionally draws again with the
// severed-head palette, and uploads the game's palette back so cgGL's shadow state stays true. Draws only: the
// simulation, Wormsign and physics never see a pose, so this cannot desync. With no pose live every draw goes
// straight through.
//
// The draw is trusted to be the Cg program last bound only while that program's ARB id is still bound and
// GL_VERTEX_PROGRAM_ARB is enabled (glBindProgramARB / glEnable / glDisable are tracked through the hub): after
// cgGLDisableProfile or a fixed-function draw nothing is read or patched.
//
// Rigid head attachments (hat, moustache, glasses) are separate unskinned draws (FFVertexMain*: world = v * model).
// With hide.attachments, a draw whose `model` origin lies within 9 units of the posed worm's unposed head bones is
// skipped ("hide", and always under a stump) or drawn with the head's pose delta applied to `model` ("follow"). The
// engine-held weapon is not covered: give a posed tool an invisible held mesh and draw it with wum.draw.mesh.
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
    const void* copyOwner = nullptr;
    uint64_t copyUntil = 0;
    lg::M4 head = lg::Identity();
    float bones[lg::kBones][3] = {};
    uint64_t bonesFrame = 0;
    // Head attachments: the unposed head bone pivots of the last draw and the head group's pose delta.
    float headPts[lg::kHeadBones][3] = {};
    uint64_t headFrame = 0;
    lg::M4 headDelta = lg::Identity();
};
Slot g_slots[16];
int g_live = 0;        // slots with a live pose or copy (recounted every frame)
int g_attachLive = 0;  // live poses with an attachment rule
uint64_t g_countFrame = ~0ull;
lg::Centre g_centre;

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
    g_live = g_attachLive = 0;
    const bool match = sim::InMatch();
    for (Slot& s : g_slots) {
        if (s.owner && (!match || Now() > s.until)) s.owner = nullptr;
        if (s.copyOwner && (!match || Now() > s.copyUntil)) s.copyOwner = nullptr;
        g_live += (s.owner || s.copyOwner) ? 1 : 0;
        g_attachLive += s.owner && s.spec.attach != lg::Attach::Keep ? 1 : 0;
    }
}

// Once per frame while anything is live: expired poses go, and every pose goes at match end, so a finished pose
// stops the per-draw readback.
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

// The draw really runs the Cg vertex program last bound (see the header comment).
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
    if (pose && !s.spec.Empty()) {
        lg::Compose(pal, s.spec, out, &s.headDelta);
        Upload(g_vp->base, out);
        uploaded = true;
        draw();
        for (int b = 0; b < lg::kBones; ++b) lg::PivotWorld(out, b, s.bones[b]);
    } else {
        s.headDelta = lg::Identity();
        draw();
        for (int b = 0; b < lg::kBones; ++b) lg::PivotWorld(pal, b, s.bones[b]);
    }
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
    // pose.test <slot> prone|stump|mount|copy|off : a 250-frame pose (about 4 s at 60 fps) for screenshots, owned by
    // the console.
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
    }
    const Result r = Set(&kOwner, slot, s, 250);
    LOG_INFO("[pose] pose.test %d %s: %s (status: %s, centre %.2f from %d sample(s))", slot, what,
             r == Result::Ok ? "ok" : "refused", Status(), g_centre.y, g_centre.samples);
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
    g_vp->arbId = g_arbBound;  // cgGL's glBindProgramARB went through HkBindProgram just now (or was already bound)
    Resolve(program, *g_vp);
    if (g_vp->skinned) g_sawSkinned = true;
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
    if (g_getTried && !g_getLocal) return "unsupported";
    if (!g_sawSkinned) return "no skinned worm drawn yet";
    if (!g_identified) return "untested";
    return "ok";
}

uint32_t IdentifiedDraws() { return g_identified; }

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
