#include "render/mirage/engine.h"

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"

namespace melange::mirage::engine {
namespace {
constexpr uintptr_t kRmPtr = 0x9796f0, kShaderMgrPtr = 0x984ce0, kPostProcessPtr = 0x961d7c, kAppOptionsPtr = 0x95a100;
constexpr uintptr_t kCreateObject = 0x639b83, kSceneFuncClass = 0x8905bc;
constexpr uintptr_t kSetPreBucketFunc = 0x6ebd7e, kSetPostBucketFunc = 0x6ebd9d, kBucketDispatch = 0x6df948,
                    kComposite = 0x61d6b0, kBeginScene = 0x61f7a0, kCgBind = 0x7988b0;

int g_state = -1;

uintptr_t Rd(uintptr_t a) {
    uintptr_t v = 0;
    mem::SafeRead(a, &v, 4);
    return v;
}
uint8_t Rd8(uintptr_t a) {
    uint8_t v = 0;
    mem::SafeRead(a, &v, 1);
    return v;
}
}  // namespace

bool Check() {
    if (g_state >= 0) return g_state == 1;
    bool ok = game::IsKnownBuild();
    ok = ok && mem::Expect(kCreateObject, {0xE8, 0x95, 0xFF, 0xFF, 0xFF});
    // SetPre/PostBucketFunc read the slot arrays at sort+0x1a4 / sort+0x1b0
    ok = ok && mem::Expect(kSetPreBucketFunc, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x80, 0xA4, 0x01, 0x00, 0x00});
    ok = ok && mem::Expect(kSetPostBucketFunc, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x80, 0xB0, 0x01, 0x00, 0x00});
    // call [eax+0x14] with (a1, sceneFunc), cdecl
    ok = ok && mem::Expect(kBucketDispatch, {0xFF, 0x50, 0x14, 0x59, 0x59});
    ok = ok && mem::Expect(kComposite, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF});
    // cmp byte [*0x95a100 + 0x74], 0: the FXAA flag
    ok = ok && mem::Expect(kBeginScene, {0xA1, 0x00, 0xA1, 0x95, 0x00, 0x80, 0x78, 0x74, 0x00});
    // cmp byte [prog+0x44], 0: the CGProg reload flag
    ok = ok && mem::Expect(kCgBind, {0x57, 0x8B, 0x7C, 0x24, 0x08, 0x80, 0x7F, 0x44, 0x00});
    g_state = ok ? 1 : 0;
    if (!ok) LOG_ERROR("[mirage] engine address checks failed: renderer access disabled");
    return ok;
}

uintptr_t Rm() { return Check() ? Rd(kRmPtr) : 0; }

uintptr_t Sort() {
    uintptr_t rm = Rm();
    return rm ? Rd(rm + 0x54) : 0;
}

uintptr_t Cam() {
    uintptr_t rm = Rm();
    uintptr_t p = rm ? Rd(rm + 0x60) : 0;
    return p ? Rd(p) : 0;
}

uintptr_t ShaderMgr() { return Check() ? Rd(kShaderMgrPtr) : 0; }
uintptr_t PostProcess() { return Check() ? Rd(kPostProcessPtr) : 0; }

int Pass() {
    if (g_state != 1) return 0;
    // plain reads: called from the GL log stub; the render manager lives for the whole process once created
    uintptr_t rm = *reinterpret_cast<const volatile uintptr_t*>(kRmPtr);
    return rm ? *reinterpret_cast<const volatile int*>(rm + 0x100) : 0;
}

int BucketCount() {
    uintptr_t sort = Sort();
    if (!sort) return 0;
    uintptr_t b = Rd(sort + 0x174), e = Rd(sort + 0x178);
    return e > b ? static_cast<int>((e - b) / 16) : 0;
}

uintptr_t CreateSceneFunc(void(__cdecl* fn)(void*, void*)) {
    if (!Check() || !fn) return 0;
    auto create = reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t)>(kCreateObject);
    uintptr_t obj = create(kSceneFuncClass);
    if (!obj) return 0;
    auto addRef = reinterpret_cast<unsigned long(__stdcall*)(uintptr_t)>(Rd(Rd(obj) + 4));
    addRef(obj);
    *reinterpret_cast<uintptr_t*>(obj + 0x14) = reinterpret_cast<uintptr_t>(fn);
    return obj;
}

bool SetBucketFunc(int id, bool post, uintptr_t sceneFunc) {
    uintptr_t rm = Rm();
    if (!rm || id < 0 || id >= BucketCount()) return false;
    auto set = reinterpret_cast<int(__stdcall*)(uintptr_t, int, uintptr_t)>(Rd(Rd(rm) + (post ? 0x94 : 0x90)));
    if (!set) return false;
    set(rm, id, sceneFunc);
    return BucketFunc(id, post) == sceneFunc;
}

uintptr_t BucketFunc(int id, bool post) {
    uintptr_t sort = Sort();
    if (!sort || id < 0 || id >= BucketCount()) return 0;
    uintptr_t arr = Rd(sort + (post ? 0x1b0 : 0x1a4));
    return arr ? Rd(arr + static_cast<uintptr_t>(id) * 4) : 0;
}

int ForEachCgProg(void (*fn)(const CgProg&, void*), void* user) {
    uintptr_t mgr = ShaderMgr();
    if (!mgr || !fn) return 0;
    uintptr_t b = Rd(mgr + 0x368), e = Rd(mgr + 0x36c);
    int n = 0;
    for (uintptr_t it = b; it && it < e && n < 4096; it += 4) {
        uintptr_t p = Rd(it);
        if (!p) continue;
        CgProg c{};
        c.self = p;
        c.program = Rd(p + 0x10);
        c.type = static_cast<int>(Rd(p + 0xc));
        c.path = reinterpret_cast<const char*>(Rd(p + 0x3c));
        c.entry = reinterpret_cast<const char*>(Rd(p + 0x40));
        c.reload = Rd8(p + 0x44) != 0;
        c.failed = Rd8(p + 0x45) != 0;
        c.binds = static_cast<uint32_t>(Rd(p + 0x38));
        fn(c, user);
        ++n;
    }
    return n;
}

bool MarkReload(uintptr_t cgprog) {
    if (!Check() || !cgprog) return false;
    uint8_t v[2] = {1, 0};
    return mem::Write(cgprog + 0x44, v, 2);
}

uintptr_t CgContext() {
    uintptr_t mgr = ShaderMgr();
    return mgr ? Rd(mgr + 0x14) : 0;
}

int CgProfile(int type) {
    uintptr_t mgr = ShaderMgr();
    return mgr ? static_cast<int>(Rd(mgr + (type == 0 ? 0x18 : 0x1c))) : 0;
}

bool FxaaOn() {
    uintptr_t o = Check() ? Rd(kAppOptionsPtr) : 0;
    return o && Rd8(o + 0x74) != 0;
}

// Composite reads the flag every frame and picks CopyFxaa at 1x1 SSAA; other SSAA factors resize the targets.
bool SetFxaa(bool on) {
    uintptr_t o = Check() ? Rd(kAppOptionsPtr) : 0;
    if (!o || Rd(o + 0x6c) != 1 || Rd(o + 0x70) != 1) return false;
    *reinterpret_cast<volatile uint8_t*>(o + 0x74) = on ? 1 : 0;
    return true;
}

bool MsaaOn() {
    uintptr_t pp = PostProcess();
    return pp && Rd8(pp + 0x7a) != 0;
}
}  // namespace melange::mirage::engine
