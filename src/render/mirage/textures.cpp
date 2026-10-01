// Module "MirageTextures" (L1 "texture clarity"): anisotropic filtering, trilinear min filter and an optional LOD
// bias, applied at the engine's own texture upload. Off by default (vanilla behaviour); a client-only mod asks for
// it through spice.json's "graphics" block, and [MirageTextures] in Melange.ini always has the last word.
#include <windows.h>
#include <GL/gl.h>
#include <intrin.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/compat.h"
#include "melange/mods.h"
#include "melange/testcmd.h"
#include "render/mirage/engine.h"
#include "render/mirage/hub.h"
#include "render/mirage/stages.h"
#include "render/mirage/textures_logic.h"

namespace melange::mirage::textures {
namespace {
namespace logic = melange::mirage::textures::logic;
constexpr GLenum kMaxAnisotropy = 0x84FE, kMaxMaxAnisotropy = 0x84FF, kLodBias = 0x8501;
// The engine's texture loader (build #1077), same function texdump.cpp taps: cdecl(XImage*, char is3D, GLenum
// target), whose glTexImage2D calls return into [kUpload, kUploadEnd). Shared knowledge, not shared code: each file
// that leans on it checks its own prologue bytes before trusting the address.
constexpr uintptr_t kUpload = 0x79dc50, kUploadEnd = 0x79df90;

logic::IniOverride g_ini;
logic::Effective g_effective;
int g_maxAnisoPolicy = 16;  // the ini-level cap; the real driver cap is applied at glTexParameter time
bool g_hooksInstalled = false, g_hooksFailed = false;

void(APIENTRY* n_texImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
void(APIENTRY* n_texParameteri)(GLenum, GLenum, GLint) = nullptr;

std::mutex g_mx;
std::unordered_set<GLuint> g_known;
constexpr size_t kMaxKnown = 16384;

float MaxAnisoSupported() {
    static float cached = -1.f;
    if (cached < 0.f) {
        while (glGetError() != GL_NO_ERROR) {
        }
        GLfloat m = 1.f;
        glGetFloatv(kMaxMaxAnisotropy, &m);
        cached = (glGetError() == GL_NO_ERROR && m >= 1.f) ? m : 1.f;
    }
    return cached;
}

// Sets every parameter explicitly (including the "off" values), so a texture already touched once can be returned
// to vanilla behaviour if the effective settings later turn off.
void ApplyToBound(const logic::Effective& e) {
    float cap = MaxAnisoSupported();
    float aniso = e.anisotropy > 0 ? std::min(static_cast<float>(e.anisotropy), cap) : 1.f;
    if (cap > 1.f) glTexParameterf(GL_TEXTURE_2D, kMaxAnisotropy, aniso);
    n_texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, e.trilinear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, kLodBias, e.lodBias);
}

void RememberBound() {
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (!bound) return;
    std::lock_guard lk(g_mx);
    if (g_known.size() < kMaxKnown) g_known.insert(static_cast<GLuint>(bound));
}

// The engine frees textures and reuses their names, often for single-level HUD textures, so a remembered name is
// only touched while it still has a mip chain.
bool HasMipChain(GLuint n) {
    if (!glIsTexture(n)) return false;
    glBindTexture(GL_TEXTURE_2D, n);
    if (glGetError() != GL_NO_ERROR) return false;
    GLint w1 = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w1);
    return w1 > 0;
}

// Hooks installed after start (a mod enabled live) never saw the textures already loaded: adopt every live 2D texture
// that has a mip chain.
void DiscoverLoaded() {
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    while (glGetError() != GL_NO_ERROR) {
    }
    std::lock_guard lk(g_mx);
    for (GLuint n = 1; n < 32768 && g_known.size() < kMaxKnown; ++n)
        if (HasMipChain(n)) g_known.insert(n);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
    while (glGetError() != GL_NO_ERROR) {
    }
}

void Resweep() {
    if (!g_hooksInstalled || !wglGetCurrentContext()) return;
    std::vector<GLuint> names, stale;
    {
        std::lock_guard lk(g_mx);
        names.assign(g_known.begin(), g_known.end());
    }
    if (names.empty()) return;
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    while (glGetError() != GL_NO_ERROR) {
    }
    for (GLuint n : names) {
        if (HasMipChain(n)) ApplyToBound(g_effective);
        else stale.push_back(n);
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
    while (glGetError() != GL_NO_ERROR) {
    }
    std::lock_guard lk(g_mx);
    for (GLuint n : stale) g_known.erase(n);
}

void APIENTRY HkTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void* pixels) {
    uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    n_texImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    // g_hooksInstalled also guards n_texParameteri: ApplyToBound needs both hooks, not just this one, chained.
    // Keyed on level 1 so only textures with a mip chain get a mipmapped min filter: on a single-level texture
    // (HUD, fonts) it would make the texture incomplete and it would draw blank.
    if (g_hooksInstalled && target == GL_TEXTURE_2D && level == 1 && caller >= kUpload && caller < kUploadEnd) {
        ApplyToBound(g_effective);
        RememberBound();
    }
}

// Pins the min filter against the engine re-asserting its own vanilla value on a texture we already touched (and on
// any other texture we didn't see at upload, harmlessly matching its own default).
void APIENTRY HkTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER && g_effective.trilinear && param == GL_LINEAR_MIPMAP_NEAREST)
        param = GL_LINEAR_MIPMAP_LINEAR;
    n_texParameteri(target, pname, param);
}

bool NonVanilla(const logic::Effective& e) { return e.anisotropy > 0 || e.trilinear || e.lodBias != 0.f; }

std::string Describe(const logic::Effective& e) {
    char b[160];
    snprintf(b, sizeof b, "anisotropy=%d trilinear=%d lodBias=%.2f (%s)", e.anisotropy, e.trilinear, e.lodBias,
             e.anyModRequest ? "mod-requested" : NonVanilla(e) ? "ini override" : "vanilla");
    return b;
}

void TryInstallHooks() {
    if (g_hooksInstalled || g_hooksFailed) return;
    if (!game::IsKnownBuild() || !engine::Check() ||
        !mem::Expect(kUpload, {0x83, 0xEC, 0x28, 0x55, 0x56}) ||
        (!hub::Installed() && !hub::Require("MirageTextures"))) {
        g_hooksFailed = true;
        LOG_WARN("[mirage] [MirageTextures] cannot hook the texture uploader; texture clarity stays off");
        compat::Report(compat::Kind::Feature, "texclarity", compat::Status::Failed, "engine upload function not recognised or GL hub unavailable",
                       "builtin");
        return;
    }
    if (!hub::Interpose("glTexImage2D", reinterpret_cast<void*>(&HkTexImage2D), reinterpret_cast<void**>(&n_texImage2D)) ||
        !hub::Interpose("glTexParameteri", reinterpret_cast<void*>(&HkTexParameteri), reinterpret_cast<void**>(&n_texParameteri))) {
        g_hooksFailed = true;
        LOG_WARN("[mirage] [MirageTextures] hook installation failed; texture clarity stays off");
        compat::Report(compat::Kind::Feature, "texclarity", compat::Status::Failed, "hook installation failed", "builtin");
        return;
    }
    g_hooksInstalled = true;
    compat::Report(compat::Kind::Feature, "texclarity", compat::Status::Loaded, Describe(g_effective).c_str(), "builtin");
}

void Recompute() {
    std::vector<logic::ModRequest> reqs;
    mods::ModInfo info[256];
    int n = mods::List(info, 256);
    for (int i = 0; i < n; ++i) {
        if (info[i].state != mods::State::Enabled) continue;
        mods::GraphicsRequest gr{};
        if (!mods::GetGraphicsRequest(info[i].id, &gr) || !gr.present) continue;
        reqs.push_back({true, gr.trilinearFilter, gr.lodBiasSet, gr.anisotropy, gr.lodBias});
    }
    g_effective = logic::Merge(g_ini, reqs, g_maxAnisoPolicy);
}

void OnModsChanged(void*) {
    logic::Effective before = g_effective;
    Recompute();
    if (before.anisotropy == g_effective.anisotropy && before.trilinear == g_effective.trilinear && before.lodBias == g_effective.lodBias)
        return;
    LOG_INFO("[mirage] [MirageTextures] effective settings changed: %s", Describe(g_effective).c_str());
    if (NonVanilla(g_effective) && !g_hooksInstalled) {
        TryInstallHooks();
        if (g_hooksInstalled && wglGetCurrentContext()) DiscoverLoaded();
    }
    if (g_hooksInstalled) Resweep();
}

bool VerbInfo(std::string_view, void*) {
    size_t known;
    {
        std::lock_guard lk(g_mx);
        known = g_known.size();
    }
    LOG_INFO("[mirage] texclarity: %s hooks=%d known-textures=%zu driver-max-aniso=%.0f", Describe(g_effective).c_str(),
             g_hooksInstalled, known, MaxAnisoSupported());
    return true;
}

class MirageTextures final : public melange::Module {
public:
    const char* Name() const override { return "MirageTextures"; }
    const char* Description() const override { return "texture clarity: anisotropic filtering, trilinear min filter, LOD bias"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 46; }

    bool Install() override {
        if (!melange::mirage::stages::CoreEnabled(Name())) return false;
        std::string as = String("Anisotropy", "auto"), ts = String("Trilinear", "auto"), ls = String("LodBias", "auto");
        if (!logic::ParseAnisotropy(as, 16, &g_ini.anisotropy)) {
            LOG_WARN("[mirage] [MirageTextures] Anisotropy=%s not understood; using auto", as.c_str());
            g_ini.anisotropy = -1;
        }
        if (!logic::ParseTriState(ts, &g_ini.trilinear)) {
            LOG_WARN("[mirage] [MirageTextures] Trilinear=%s not understood; using auto", ts.c_str());
            g_ini.trilinear = -1;
        }
        if (!logic::ParseLodBias(ls, 8.f, &g_ini.lodBiasSet, &g_ini.lodBias)) {
            LOG_WARN("[mirage] [MirageTextures] LodBias=%s not understood; using auto", ls.c_str());
            g_ini.lodBiasSet = false;
        }
        Recompute();
        melange::testcmd::Register("mirage.textures", &VerbInfo);
        melange::mods::OnChange(&OnModsChanged, nullptr);
        if (!NonVanilla(g_effective)) {
            compat::Report(compat::Kind::Feature, "texclarity", compat::Status::Skipped, "no mod request and no override", "builtin");
            LOG_INFO("[mirage] texture clarity: off (vanilla; no mod request and no [MirageTextures] override)");
            return true;
        }
        TryInstallHooks();
        LOG_INFO("[mirage] texture clarity: %s", Describe(g_effective).c_str());
        return true;
    }

private:
    std::string String(const char* key, const char* def) const {
        melange::config::EnsureKey(Name(), key, def);
        return melange::config::GetString(Name(), key, def);
    }
};
}  // namespace

MELANGE_MODULE(MirageTextures);
}  // namespace melange::mirage::textures
