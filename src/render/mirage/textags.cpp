// Module "MirageTexTags": texture tags. Enabled mods' spice.json "graphics": {"textureTags": "<file>"} tables map a
// digest of a texture's base level (textags_logic.h) to 8 numbers. Melange hashes level 0 of every texture the engine's
// loader uploads (glTexImage2D / glCompressedTexImage2D called from 0x79dc50), keeps GL name -> tag, follows
// glDeleteTextures and name reuse, tracks the 2D binding of each texture unit, and hands the tag of the texture on a
// replaced GLSL program's texture0 unit to its `uniform vec4 mg_texTag[2]` (shaders_glsl.cpp). A texture it never saw
// uploaded is read back once with glGetTexImage the first time a replaced program draws with it. Nothing is hooked
// unless an enabled mod ships a table: no table, no cost.
#include <windows.h>
#include <GL/gl.h>
#include <intrin.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/compat.h"
#include "melange/mods.h"
#include "melange/testcmd.h"
#include "render/mirage/hub.h"
#include "render/mirage/stages.h"
#include "render/mirage/textags.h"
#include "render/mirage/textags_logic.h"

namespace melange::mirage::textags {
namespace {
namespace lg = logic;
// The engine's texture loader (build #1077): its glTexImage2D / glCompressedTexImage2D calls return into
// [kUpload, kUploadEnd). Same knowledge as textures.cpp and texdump.cpp.
constexpr uintptr_t kUpload = 0x79dc50, kUploadEnd = 0x79df90;
constexpr GLenum kTexture0 = 0x84C0, kActiveTextureEnum = 0x84E0, kInternalFormat = 0x1003, kCompressed = 0x86A1,
                 kCompressedSize = 0x86A0, kPackBuffer = 0x88ED, kUnpackBuffer = 0x88EF, kUnpackRowLength = 0x0CF2,
                 kUnpackSkipRows = 0x0CF3, kUnpackSkipPixels = 0x0CF4;
constexpr int kMaxSide = 8192;

lg::Table g_table;
lg::Tracker g_tr;
bool g_hooks = false, g_hooksFailed = false, g_warnedRestart = false;
bool g_active = false;  // hooks in and a non-empty table

struct Stats {
    uint64_t hashed = 0, hashedTagged = 0, readBacks = 0, readBacksTagged = 0, readBackFailed = 0, notHashable = 0;
    uint64_t drawsTagged = 0, drawsUntagged = 0, distinctTagged = 0, distinctUntagged = 0;
} g_st;
const float kZero[lg::kTagFloats] = {};

struct Cache {
    uint32_t name = lg::Tracker::kUnknown, gen = 0;
    const float* tag = kZero;
    bool tagged = false;
} g_cache;

using TexImage2D = void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using CompressedTexImage2D = void(APIENTRY*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);
using TexSubImage2D = void(APIENTRY*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
using DeleteTextures = void(APIENTRY*)(GLsizei, const GLuint*);
using BindTexture = void(APIENTRY*)(GLenum, GLuint);
using ActiveTexture = void(APIENTRY*)(GLenum);
using GetCompressedTexImage = void(APIENTRY*)(GLenum, GLint, void*);
TexImage2D n_texImage2D;
TexSubImage2D n_texSubImage2D;
CompressedTexImage2D n_compressed, n_compressedArb;
DeleteTextures n_deleteTextures;
BindTexture n_bindTexture;
ActiveTexture n_activeTexture, n_activeTextureArb;
// Melange's own entry points (its imports are not routed through the hub): used by the read-back, which restores
// everything it changes.
ActiveTexture p_activeTexture;
GetCompressedTexImage p_getCompressed;

void DrainErrors() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }
}

GLuint BoundNow() {
    GLint b = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &b);
    return static_cast<GLuint>(b);
}

void NoteHashed(GLuint name, uint64_t d, GLsizei w, GLsizei h, bool readBack) {
    g_tr.Hashed(name, d, static_cast<uint16_t>(w), static_cast<uint16_t>(h), g_table);
    const lg::Tracker::Entry* e = g_tr.Find(name);
    bool tagged = e && e->row >= 0;
    if (readBack) ++g_st.readBacks, g_st.readBacksTagged += tagged;
    else ++g_st.hashed, g_st.hashedTagged += tagged;
}

void APIENTRY HkTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void* pixels) {
    const uintptr_t caller = hub::RealCaller(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    n_texImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    if (target != GL_TEXTURE_2D || level != 0) return;
    const GLuint name = BoundNow();
    if (!name) return;
    const bool loader = caller >= kUpload && caller < kUploadEnd;
    if (!loader || !pixels || !g_active) {
        // Re-specified by anything else (render targets, dynamic textures), or uploaded while no table is loaded:
        // the name keeps no tag. Loader uploads with no table are forgotten so a later table can read them back.
        if (loader) g_tr.Forget(name);
        else g_tr.Dynamic(name);
        return;
    }
    const int bpp = lg::BytesPerPixel(format, type);
    GLint align = 4, rowLen = 0, skipRows = 0, skipPix = 0, unpackBuf = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGetIntegerv(kUnpackRowLength, &rowLen);
    glGetIntegerv(kUnpackSkipRows, &skipRows);
    glGetIntegerv(kUnpackSkipPixels, &skipPix);
    glGetIntegerv(kUnpackBuffer, &unpackBuf);
    uint64_t d = 0;
    if (!bpp || skipRows || skipPix || unpackBuf || width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide ||
        !lg::Digest(static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(internalformat), format,
                    type, static_cast<const uint8_t*>(pixels), static_cast<size_t>(width) * static_cast<size_t>(bpp),
                    lg::SourceStride(width, bpp, align, rowLen), &d)) {
        ++g_st.notHashable;
        g_tr.Dynamic(name);
        return;
    }
    NoteHashed(name, d, width, height, false);
}

void OnCompressed(CompressedTexImage2D next, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                  GLint border, GLsizei imageSize, const void* data, uintptr_t caller) {
    next(target, level, internalformat, width, height, border, imageSize, data);
    if (target != GL_TEXTURE_2D || level != 0) return;
    const GLuint name = BoundNow();
    if (!name) return;
    const bool loader = caller >= kUpload && caller < kUploadEnd;
    if (!loader || !data || !g_active) {
        if (loader) g_tr.Forget(name);
        else g_tr.Dynamic(name);
        return;
    }
    GLint unpackBuf = 0;
    glGetIntegerv(kUnpackBuffer, &unpackBuf);
    uint64_t d = 0;
    if (unpackBuf || imageSize <= 0 || imageSize > (64 << 20) ||
        !lg::DigestCompressed(static_cast<uint32_t>(width), static_cast<uint32_t>(height), internalformat,
                              static_cast<const uint8_t*>(data), static_cast<size_t>(imageSize), &d)) {
        ++g_st.notHashable;
        g_tr.Dynamic(name);
        return;
    }
    NoteHashed(name, d, width, height, false);
}

void APIENTRY HkCompressed(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void* p) {
    OnCompressed(n_compressed, t, l, f, w, h, b, n, p, hub::RealCaller(reinterpret_cast<uintptr_t>(_ReturnAddress())));
}
void APIENTRY HkCompressedArb(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void* p) {
    OnCompressed(n_compressedArb, t, l, f, w, h, b, n, p, hub::RealCaller(reinterpret_cast<uintptr_t>(_ReturnAddress())));
}

// A level 0 changed in place no longer matches its digest.
void APIENTRY HkTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
                              const void* pixels) {
    n_texSubImage2D(target, level, x, y, w, h, format, type, pixels);
    if (target == GL_TEXTURE_2D && level == 0)
        if (const GLuint name = BoundNow()) g_tr.Dynamic(name);
}

void APIENTRY HkDeleteTextures(GLsizei n, const GLuint* names) {
    if (names)
        for (GLsizei i = 0; i < n; ++i) g_tr.Delete(names[i]);
    n_deleteTextures(n, names);
}

void APIENTRY HkBindTexture(GLenum target, GLuint name) {
    if (target == GL_TEXTURE_2D) g_tr.Bind(name);
    n_bindTexture(target, name);
}

void APIENTRY HkActiveTexture(GLenum unit) {
    g_tr.ActiveTexture(unit - kTexture0);
    n_activeTexture(unit);
}
void APIENTRY HkActiveTextureArb(GLenum unit) {
    g_tr.ActiveTexture(unit - kTexture0);
    n_activeTextureArb(unit);
}

// The one-off fallback for a texture Melange never saw uploaded: hash level 0 as the driver gives it back. Only the
// engine's own formats can reproduce the upload digest (RGB8, RGBA8, and compressed levels byte for byte).
void ReadBack(uint32_t unit, GLuint name) {
    DrainErrors();  // so the checks below see only the read-back's own errors
    GLint prevActive = static_cast<GLint>(kTexture0);
    glGetIntegerv(kActiveTextureEnum, &prevActive);
    const bool switchUnit = static_cast<GLuint>(prevActive) != kTexture0 + unit;
    if (switchUnit && p_activeTexture) p_activeTexture(kTexture0 + unit);
    bool ok = false;
    uint64_t d = 0;
    GLint w = 0, h = 0, ifmt = 0, compressed = 0;
    if (BoundNow() == name && (!switchUnit || p_activeTexture)) {
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, kInternalFormat, &ifmt);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, kCompressed, &compressed);
        GLint packBuf = 0;
        glGetIntegerv(kPackBuffer, &packBuf);
        uint32_t fmt = 0, type = 0;
        if (w > 0 && h > 0 && w <= kMaxSide && h <= kMaxSide && !packBuf) {
            if (compressed && p_getCompressed) {
                GLint size = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, kCompressedSize, &size);
                if (size > 0 && size <= (64 << 20)) {
                    std::vector<uint8_t> buf(static_cast<size_t>(size));
                    p_getCompressed(GL_TEXTURE_2D, 0, buf.data());
                    ok = glGetError() == GL_NO_ERROR &&
                         lg::DigestCompressed(static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(ifmt),
                                              buf.data(), buf.size(), &d);
                }
            } else if (!compressed && lg::ReadbackFormat(static_cast<uint32_t>(ifmt), &fmt, &type)) {
                const int bpp = lg::BytesPerPixel(fmt, type);
                const size_t row = static_cast<size_t>(w) * static_cast<size_t>(bpp);
                std::vector<uint8_t> buf(row * static_cast<size_t>(h));
                GLint packAlign = 4, packRow = 0, packSkipR = 0, packSkipP = 0;
                glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
                glGetIntegerv(GL_PACK_ROW_LENGTH, &packRow);
                glGetIntegerv(GL_PACK_SKIP_ROWS, &packSkipR);
                glGetIntegerv(GL_PACK_SKIP_PIXELS, &packSkipP);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                glGetTexImage(GL_TEXTURE_2D, 0, fmt, type, buf.data());
                ok = glGetError() == GL_NO_ERROR &&
                     lg::Digest(static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(ifmt), fmt, type,
                                buf.data(), row, row, &d);
                glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
                glPixelStorei(GL_PACK_ROW_LENGTH, packRow);
                glPixelStorei(GL_PACK_SKIP_ROWS, packSkipR);
                glPixelStorei(GL_PACK_SKIP_PIXELS, packSkipP);
            }
        }
    }
    if (switchUnit && p_activeTexture) p_activeTexture(static_cast<GLenum>(prevActive));
    DrainErrors();
    if (ok) {
        NoteHashed(name, d, w, h, true);
    } else {
        ++g_st.readBackFailed;
        g_tr.Dynamic(name);  // not again
    }
}

GLuint QueryBound(uint32_t unit) {
    GLint prev = static_cast<GLint>(kTexture0);
    glGetIntegerv(kActiveTextureEnum, &prev);
    g_tr.ActiveTexture(static_cast<uint32_t>(prev) - kTexture0);
    if (static_cast<GLuint>(prev) == kTexture0 + unit) return BoundNow();
    if (!p_activeTexture) return lg::Tracker::kUnknown;
    p_activeTexture(kTexture0 + unit);
    GLuint b = BoundNow();
    p_activeTexture(static_cast<GLenum>(prev));
    return b;
}

void Reload() {
    lg::Table t;
    std::vector<mods::ModInfo> info(256);
    int n = std::min(mods::List(info.data(), 256), 256);
    for (int i = 0; i < n; ++i) {
        if (info[i].state != mods::State::Enabled) continue;
        wchar_t path[1024];
        if (!mods::TextureTagsPath(info[i].id, path, 1024)) continue;
        json::Value v;
        json::Error e;
        lg::Table one;
        std::string err;
        if (!json::ParseFile(path, &v, &e, 16u << 20)) err = "line " + std::to_string(e.line) + ": " + e.text;
        else lg::ParseTable(v, &one, &err);
        if (!err.empty()) {
            LOG_WARN("[textags] %s: texture-tag table %s not loaded: %s", info[i].id, game::Narrow(path).c_str(), err.c_str());
            compat::Report(compat::Kind::Feature, "textags", compat::Status::Failed, err.c_str(), info[i].id);
            continue;
        }
        size_t dupes = 0, added = lg::Merge(&t, one, info[i].id, &dupes);
        LOG_INFO("[textags] %s: %zu texture tags (%zu already declared by an earlier mod)", info[i].id, added, dupes);
    }
    g_table = std::move(t);
    g_tr.Retag(g_table);
    g_cache = {};
}

bool InstallHooks(bool atStart) {
    if (g_hooks || g_hooksFailed) return g_hooks;
    if (!hub::Installed() && !(atStart && hub::Require("MirageTexTags"))) {
        if (!g_warnedRestart) LOG_WARN("[textags] a texture-tag table was enabled after start; texture tags need a restart");
        g_warnedRestart = true;
        return false;
    }
    auto chain = [](const char* name, auto hook, auto* next) {
        return hub::Interpose(name, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(next));
    };
    bool ok = chain("glTexImage2D", &HkTexImage2D, &n_texImage2D) && chain("glTexSubImage2D", &HkTexSubImage2D, &n_texSubImage2D) &&
              chain("glDeleteTextures", &HkDeleteTextures, &n_deleteTextures) &&
              chain("glBindTexture", &HkBindTexture, &n_bindTexture) && chain("glActiveTexture", &HkActiveTexture, &n_activeTexture) &&
              chain("glActiveTextureARB", &HkActiveTextureArb, &n_activeTextureArb) &&
              chain("glCompressedTexImage2D", &HkCompressed, &n_compressed) &&
              chain("glCompressedTexImage2DARB", &HkCompressedArb, &n_compressedArb);
    if (!ok) {
        g_hooksFailed = true;
        LOG_WARN("[textags] hook installation failed; texture tags stay off");
        compat::Report(compat::Kind::Feature, "textags", compat::Status::Failed, "hook installation failed", "builtin");
        return false;
    }
    // Hooked before the game's context exists, every binding is known to be 0; hooked later, each unit's binding is
    // read once when first needed.
    g_tr.ResetBindings(atStart);
    g_hooks = true;
    return true;
}

void LoadProcs() {
    static bool tried = false;
    if (tried || !wglGetCurrentContext()) return;
    tried = true;
    p_activeTexture = reinterpret_cast<ActiveTexture>(wglGetProcAddress("glActiveTexture"));
    p_getCompressed = reinterpret_cast<GetCompressedTexImage>(wglGetProcAddress("glGetCompressedTexImage"));
    if (!p_getCompressed) p_getCompressed = reinterpret_cast<GetCompressedTexImage>(wglGetProcAddress("glGetCompressedTexImageARB"));
}

void Apply(bool atStart) {
    Reload();
    if (!g_table.tags.empty() && !g_hooks) InstallHooks(atStart);
    const bool was = g_active;
    g_active = g_hooks && !g_table.tags.empty();
    if (g_active != was || atStart)
        LOG_INFO("[textags] texture tags %s: %zu tags from %zu mod(s)", g_active ? "on" : "off", g_table.tags.size(), g_table.sources.size());
    if (g_active) compat::Report(compat::Kind::Feature, "textags", compat::Status::Loaded,
                                 (std::to_string(g_table.tags.size()) + " tags").c_str(), "builtin");
}

void OnModsChanged(void*) { Apply(false); }

bool VerbStats(std::string_view args, void*) {
    if (args.find("reset") != std::string_view::npos) {
        g_st.drawsTagged = g_st.drawsUntagged = g_st.distinctTagged = g_st.distinctUntagged = 0;
        for (const auto& [n, e] : g_tr.Names()) {
            if (auto* m = g_tr.FindMut(n)) m->drawnTagged = m->drawnUntagged = false;
        }
        LOG_INFO("[textags] tags.stats: draw counters reset");
        return true;
    }
    size_t known = 0, tagged = 0, dynamic = 0;
    for (const auto& [n, e] : g_tr.Names()) {
        known += e.hashed;
        tagged += e.hashed && e.row >= 0;
        dynamic += e.dynamic;
    }
    LOG_INFO("[textags] tags.stats: %s, table %zu tags from %zu mod(s); live textures hashed %zu, tagged %zu, untagged %zu, "
             "dynamic %zu; uploads hashed %llu (tagged %llu), read back %llu (tagged %llu, failed %llu), not hashable %llu; "
             "draws tagged %llu, untagged %llu; distinct textures drawn tagged %llu, untagged %llu",
             g_active ? "on" : g_hooks ? "off (no table)" : "off (not hooked)", g_table.tags.size(), g_table.sources.size(),
             known, tagged, known - tagged, dynamic, static_cast<unsigned long long>(g_st.hashed),
             static_cast<unsigned long long>(g_st.hashedTagged), static_cast<unsigned long long>(g_st.readBacks),
             static_cast<unsigned long long>(g_st.readBacksTagged), static_cast<unsigned long long>(g_st.readBackFailed),
             static_cast<unsigned long long>(g_st.notHashable), static_cast<unsigned long long>(g_st.drawsTagged),
             static_cast<unsigned long long>(g_st.drawsUntagged), static_cast<unsigned long long>(g_st.distinctTagged),
             static_cast<unsigned long long>(g_st.distinctUntagged));
    // The textures a replaced program drew without a tag, so a table builder can see what it missed.
    int listed = 0;
    for (const auto& [n, e] : g_tr.Names()) {
        if (!e.drawnUntagged || listed >= 32) continue;
        ++listed;
        if (e.hashed) LOG_INFO("[textags]   untagged: texture %u %ux%u digest %s", n, e.w, e.h, lg::Hex64(e.digest).c_str());
        else LOG_INFO("[textags]   untagged: texture %u (%s)", n, e.dynamic ? "not from the engine loader or not hashable" : "unknown");
    }
    return true;
}

class MirageTexTags final : public melange::Module {
public:
    const char* Name() const override { return "MirageTexTags"; }
    const char* Description() const override { return "texture tags: per-texture numbers for GLSL replacements (mg_texTag)"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 45; }

    bool Install() override {
        if (!melange::mirage::stages::CoreEnabled(Name())) return false;
        melange::testcmd::Register("tags.stats", &VerbStats);
        melange::mods::OnChange(&OnModsChanged, nullptr);
        Apply(true);
        return true;
    }
};
}  // namespace

bool Active() { return g_active; }

const float* ForDraw(uint32_t unit) {
    if (!g_active) return kZero;
    uint32_t name = g_tr.Bound(unit);
    if (name == lg::Tracker::kUnknown) {
        LoadProcs();
        name = QueryBound(unit);
        if (name == lg::Tracker::kUnknown) {
            ++g_st.drawsUntagged;
            return kZero;
        }
        g_tr.SetBound(unit, name);
    }
    if (name != g_cache.name || g_tr.Gen() != g_cache.gen) {
        const lg::Tracker::Entry* e = name ? g_tr.Find(name) : nullptr;
        if (!e && name) {
            LoadProcs();
            ReadBack(unit, name);
            e = g_tr.Find(name);
        }
        g_cache.name = name;
        g_cache.gen = g_tr.Gen();
        g_cache.tagged = e && e->row >= 0 && static_cast<size_t>(e->row) < g_table.tags.size();
        g_cache.tag = g_cache.tagged ? g_table.tags[static_cast<size_t>(e->row)].data() : kZero;
        if (lg::Tracker::Entry* m = name ? g_tr.FindMut(name) : nullptr) {
            if (g_cache.tagged && !m->drawnTagged) m->drawnTagged = true, ++g_st.distinctTagged;
            if (!g_cache.tagged && !m->drawnUntagged) m->drawnUntagged = true, ++g_st.distinctUntagged;
        }
    }
    if (g_cache.tagged) ++g_st.drawsTagged;
    else ++g_st.drawsUntagged;
    return g_cache.tag;
}

MELANGE_MODULE(MirageTexTags);
}  // namespace melange::mirage::textags
