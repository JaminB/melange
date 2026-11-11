// LevelLandMask: the land broadphase's frame mask on maps whose base frames were removed (Erg).
//
// The landscape keeps a tree of boxes (landscape+0x98..0x9c, 0x20 bytes a node: min, max, a sphere radius at +0x18, the
// node's subtree size in nodes at +0x1c and the number of land frames under it at +0x1e). A sweep's broadphase
// (0x472960: straight and parabolic sweeps, 0x473060: sphere queries) walks it and writes one mask bit per land frame
// (landscape+0xc8), and the sweep then tests the frames whose bit is set (0x473190), taking bit k as the k-th frame of
// the land frame list (0x955740). A node that misses writes +0x1e clear bits and skips its subtree; a leaf that passes
// writes one set bit whatever its +0x1e says. In the game's own maps every leaf is a land frame (Multi.DinerMight: 517
// leaves, 517 frames). A map built on a base whose frames Erg emptied and trimmed (remove_base_frames) keeps some of
// those frames in the tree as leaves with no land frame (+0x1e = 0; Caldera: 69 of 157 leaves, at the diner's old props
// inside the volcano), so a sweep that passes one writes a bit too many and every land frame after it in the tree is
// tested in place of its neighbour: the frames the sweep crosses are never tested and it hits nothing. On Caldera
// (2026-11-10, testenv B) 66 columns of a straight drop from 3000 world units had no ground at match start, Air Strike
// bombs fell through the crater floor's skin and the shell under it to the sea, and the same miss is the mines'
// "ground a mine falls through" (docs/erg.md). A straight drop at (0.15, 0.15) map hit the crater floor when it ended
// above world y 25 and hit nothing when it reached y 0, where the tree's box of an emptied diner frame starts.
//
// The fix: at each of the four leaf sites (cmp word [reg+1Ch],1; jne <inner node>) a stub sends a passing leaf with no
// land frame down the node-missed path, which writes its +0x1e (0) bits and steps over it. A leaf with a land frame
// goes on as before, so a map whose tree has no such leaf (the game's own) sweeps exactly as unpatched. The stub only
// acts while a map from a pack or an Erg Test plays (LevelStart source not vanilla): a vanilla map keeps the game's
// own code path bit for bit whatever its tree holds. With all four sites patched on a live Caldera match the 66
// no-ground columns went to 0.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/levels.h"

namespace melange::levels::landmask {
namespace {
struct Site {
    uintptr_t at;        // cmp word [reg+1Ch],1 (5 bytes); jne short <inner> (2 bytes); then the leaf's "or"
    uint8_t modrm;       // 0x7b: [ebx+disp8], 0x7f: [edi+disp8]
    uintptr_t inner;     // the jne's target: an inner node, descend
    uintptr_t missed;    // the node-missed path: write +0x1e clear bits, step over the subtree
    uint8_t orBytes[2];  // the leaf's or <bits>, <bit> at at+7
    uint8_t missedBytes[4];
};
constexpr Site kSites[] = {
    {0x472c28, 0x7b, 0x472c48, 0x472c55, {0x0b, 0xf5}, {0x33, 0xc0, 0x33, 0xc9}},  // parabolic sweep
    {0x472d8b, 0x7b, 0x472da7, 0x472dac, {0x0b, 0xf5}, {0x33, 0xc0, 0x33, 0xc9}},  // a sweep that does not move
    {0x472f86, 0x7b, 0x472fa6, 0x472faf, {0x0b, 0xf5}, {0x33, 0xc0, 0x33, 0xc9}},  // straight sweep
    {0x473115, 0x7f, 0x473131, 0x473136, {0x0b, 0xd3}, {0x33, 0xc0, 0x33, 0xed}},  // sphere query
};
constexpr size_t kSlot = 64;

volatile uint8_t g_on = 0;  // a pack or Test level plays (main thread writes, the sim's sweeps read)
uint8_t* g_code = nullptr;
bool g_patched[sizeof kSites / sizeof kSites[0]] = {};
int g_start = 0;

void Rel32(uint8_t* p, uintptr_t from, uintptr_t to) {
    const int32_t r = static_cast<int32_t>(to - from);
    std::memcpy(p, &r, 4);
}

// The stub for one site: the original test, then (only while g_on) a leaf whose +0x1e is 0 goes the missed path.
size_t Emit(uint8_t* p, const Site& s) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(p);
    size_t n = 0;
    const uint8_t cmpLeaf[] = {0x66, 0x83, s.modrm, 0x1c, 0x01};  // cmp word [reg+1Ch],1
    std::memcpy(p + n, cmpLeaf, sizeof cmpLeaf), n += sizeof cmpLeaf;
    p[n++] = 0x0f, p[n++] = 0x85, Rel32(p + n, base + n + 4, s.inner), n += 4;  // jne inner
    p[n++] = 0x80, p[n++] = 0x3d;                                                // cmp byte [g_on],0
    const uint32_t flag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_on));
    std::memcpy(p + n, &flag, 4), n += 4;
    p[n++] = 0x00;
    p[n++] = 0x0f, p[n++] = 0x84, Rel32(p + n, base + n + 4, s.at + 7), n += 4;  // je <the leaf's or>
    const uint8_t cmpBits[] = {0x66, 0x83, s.modrm, 0x1e, 0x00};                // cmp word [reg+1Eh],0
    std::memcpy(p + n, cmpBits, sizeof cmpBits), n += sizeof cmpBits;
    p[n++] = 0x0f, p[n++] = 0x84, Rel32(p + n, base + n + 4, s.missed), n += 4;  // je missed
    p[n++] = 0xe9, Rel32(p + n, base + n + 4, s.at + 7), n += 4;                 // jmp <the leaf's or>
    return n;
}

void OnStart(const LevelStart& s, void*) {
    const uint8_t on = s.source != Source::Vanilla;
    if (on != g_on) LOG_INFO("[levels] land mask fix %s (%s)", on ? "on" : "off", s.key ? s.key : "?");
    g_on = on;
}

class LevelLandMask final : public Module {
public:
    const char* Name() const override { return "LevelLandMask"; }
    const char* Description() const override {
        return "land sweeps on maps with removed base frames: a tree leaf with no land frame no longer shifts the frame mask";
    }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 57; }
    bool Install() override {
        for (const Site& s : kSites) {
            const int8_t jne = static_cast<int8_t>(s.inner - (s.at + 7));
            if (!mem::Expect(s.at, {0x66, 0x83, s.modrm, 0x1c, 0x01, 0x75, static_cast<uint8_t>(jne), s.orBytes[0], s.orBytes[1]}) ||
                !mem::Expect(s.missed, {s.missedBytes[0], s.missedBytes[1], s.missedBytes[2], s.missedBytes[3]})) {
                LOG_WARN("[levels] land mask fix: unexpected code at %08x; not installed", static_cast<unsigned>(s.at));
                return true;
            }
        }
        g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_code) {
            LOG_ERROR("[levels] land mask fix: no memory for its stubs; not installed");
            return true;
        }
        for (size_t i = 0; i < sizeof kSites / sizeof kSites[0]; ++i) Emit(g_code + i * kSlot, kSites[i]);
        FlushInstructionCache(GetCurrentProcess(), g_code, 4096);
        size_t done = 0;
        for (size_t i = 0; i < sizeof kSites / sizeof kSites[0]; ++i) {
            g_patched[i] = mem::Jmp(kSites[i].at, g_code + i * kSlot) && mem::Nop(kSites[i].at + 5, 2);
            done += g_patched[i];
        }
        g_start = OnLevelStart(&OnStart, nullptr);
        LOG_INFO("[levels] land mask fix: %zu of %zu sites patched (acts on pack and Test levels)", done,
                 sizeof kSites / sizeof kSites[0]);
        return true;
    }
    void Uninstall() override {
        if (g_start) RemoveOnLevelStart(g_start);
        g_start = 0;
        g_on = 0;
        for (size_t i = 0; i < sizeof kSites / sizeof kSites[0]; ++i) {
            if (!g_patched[i]) continue;
            const Site& s = kSites[i];
            const uint8_t orig[] = {0x66, 0x83, s.modrm, 0x1c, 0x01, 0x75, static_cast<uint8_t>(s.inner - (s.at + 7))};
            mem::Write(s.at, orig, sizeof orig);
            g_patched[i] = false;
        }
    }
};
}  // namespace

MELANGE_MODULE(LevelLandMask);
}  // namespace melange::levels::landmask
