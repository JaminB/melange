// One-frame (1..8) GL capture into a .mcap zip (docs/capture-format.md). The main thread records and reads back;
// a worker decodes and writes.
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "melange/render.h"
#include "melange/shaders.h"
#include "render/gl_guard.h"
#include "render/mirage/engine.h"
#include "render/mirage/hub.h"
#include "render/mirage/trace_internal.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "version.h"

namespace melange::mirage::trace {
namespace {
using gltrace::CaptureOptions;
using St = gltrace::CaptureState;

constexpr GLenum kACTIVE_TEXTURE = 0x84E0, kTEXTURE0 = 0x84C0, kREAD_FRAMEBUFFER = 0x8CA8,
                 kREAD_FRAMEBUFFER_BINDING = 0x8CAA, kDRAW_FRAMEBUFFER_BINDING = 0x8CA6, kRENDERBUFFER_BINDING = 0x8CA7,
                 kPIXEL_PACK_BUFFER = 0x88EB, kPIXEL_PACK_BUFFER_BINDING = 0x88ED, kPIXEL_UNPACK_BUFFER_BINDING = 0x88EF,
                 kBLEND_EQUATION = 0x8009, kBLEND_SRC_RGB = 0x80C9, kBLEND_DST_RGB = 0x80C8, kBLEND_SRC_ALPHA = 0x80CB,
                 kBLEND_DST_ALPHA = 0x80CA, kBLEND_COLOR = 0x8005, kTEXTURE_BINDING_CUBE = 0x8514,
                 kTEXTURE_BINDING_3D = 0x806A, kCLIENT_ACTIVE_TEXTURE = 0x84E1;
constexpr uint32_t kMaxPayloadArena = 32u << 20;

struct Job : McapData {
    std::wstring path;
};
using PayRec = McapPayload;
using TexOut = McapTexture;
using ProgOut = McapProgram;

std::mutex g_mx;
St g_state = St::Idle;
std::wstring g_path;
std::string g_error;
CaptureOptions g_opt;

std::atomic<bool> g_recording{false};
Job* g_job = nullptr;
uint32_t g_readPos = 0, g_framesLeft = 0, g_ringCap = 0;
hub::Mode g_savedMode = hub::Mode::Count;
std::set<std::string> g_tapped;
int g_stageHandles[static_cast<int>(render::Stage::Count)] = {};

void SetState(St s, const std::string& err = {}) {
    std::lock_guard lk(g_mx);
    g_state = s;
    g_error = err;
}

void OnPayload(int, const uint32_t* f, void* user) {
    if (!g_recording.load(std::memory_order_relaxed) || !g_job) return;
    auto* sig = static_cast<const GlSig*>(user);
    PayloadSpec ps;
    if (!PayloadOf(*sig, f + 1, 16, &ps)) return;
    Job& j = *g_job;
    uint32_t pos = hub::RingPos() - 1;
    PayRec r{sig, 0, 0, ps.bytes, ps.hashOnly, ps.capped, {}};
    if (ps.hashOnly) {
        uint8_t head[64];
        uint32_t n = std::min<uint32_t>(ps.bytes, 64);
        if (n && !mem::SafeRead(ps.ptr, head, n)) return;
        r.hash = hashutil::Sha256Hex(head, n);
    } else {
        if (j.arena.size() + ps.bytes > kMaxPayloadArena) {
            ++j.droppedPayloads;
            return;
        }
        r.off = static_cast<uint32_t>(j.arena.size());
        j.arena.resize(j.arena.size() + ps.bytes);
        if (!mem::SafeRead(ps.ptr, j.arena.data() + r.off, ps.bytes)) {
            j.arena.resize(r.off);
            return;
        }
        r.len = ps.bytes;
    }
    j.pay[pos] = std::move(r);
}

void OnStage(render::Stage stage, void*) {
    if (!g_recording || !g_job) return;
    static const char* kNames[] = {"World", "WorldLate", "PostWorld", "Hud", "Final"};
    char b[96];
    snprintf(b, sizeof b, "\"type\":\"stage\",\"stage\":\"%s\",\"frame\":%llu", kNames[static_cast<int>(stage)],
             static_cast<unsigned long long>(events::FrameCount()));
    g_job->markers.push_back({hub::RingPos(), b});
}

void AddStageMarkers() {
    for (int s = 0; s < static_cast<int>(render::Stage::Count); ++s)
        if (!g_stageHandles[s]) g_stageHandles[s] = render::AddStageCallback(static_cast<render::Stage>(s), &OnStage, nullptr, 1000000);
}

void RemoveStageMarkers() {
    for (int& h : g_stageHandles) {
        if (h) render::RemoveStageCallback(h);
        h = 0;
    }
}

void InstallPayloadTaps() {
    for (int i = 0; i < hub::Count(); ++i) {
        const char* n = hub::Name(i);
        const GlSig* sig = FindSig(n);
        if (!sig || !sig->payKind || !g_tapped.insert(n).second) continue;
        AddTap(n, &OnPayload, const_cast<GlSig*>(sig));
    }
}

void Drain() {
    Job& j = *g_job;
    uint32_t now = hub::RingPos();
    if (now - g_readPos > g_ringCap) {
        j.dropped += now - g_readPos - g_ringCap;
        g_readPos = now - g_ringCap;
    }
    struct Ctx {
        Job* j;
        uint32_t pos;
    } c{&j, g_readPos};
    g_readPos = hub::ReadRing(g_readPos, [](const hub::Rec& r, void* u) {
        auto* c = static_cast<Ctx*>(u);
        c->j->recs.push_back(r);
        c->j->ringPos.push_back(c->pos++);
    }, &c);
}

// ---------------------------------------------------------------- GL snapshots (main thread)
GLint GetI(GLenum e) {
    GLint v[4] = {};
    glGetIntegerv(e, v);
    return v[0];
}

std::string StateJson(const char* which) {
    render::gl::Snapshot s = render::gl::Read();
    const render::gl::Caps& c = render::gl::Load();
    jsonmini::Obj v;
    char num[48];
    for (int i = 0; i < s.n; ++i) {
        snprintf(num, sizeof num, "%.9g", s.v[i]);
        v.Raw(s.name[i], num);
    }
    auto ints = [&](const char* k, GLenum e, int n) {
        GLint x[16] = {};
        glGetIntegerv(e, x);
        std::string a = "[";
        for (int i = 0; i < n; ++i) a += (i ? "," : "") + std::to_string(x[i]);
        v.Raw(k, a + "]");
    };
    auto floats = [&](const char* k, GLenum e, int n) {
        GLfloat x[16] = {};
        glGetFloatv(e, x);
        std::string a = "[";
        char b[32];
        for (int i = 0; i < n; ++i) {
            snprintf(b, sizeof b, "%s%.9g", i ? "," : "", x[i]);
            a += b;
        }
        v.Raw(k, a + "]");
    };
    floats("modelviewMatrix", GL_MODELVIEW_MATRIX, 16);
    floats("projectionMatrix", GL_PROJECTION_MATRIX, 16);
    floats("textureMatrix", GL_TEXTURE_MATRIX, 16);
    floats("currentColor", GL_CURRENT_COLOR, 4);
    floats("colorClearValue", GL_COLOR_CLEAR_VALUE, 4);
    floats("depthRange", GL_DEPTH_RANGE, 2);
    floats("depthClearValue", GL_DEPTH_CLEAR_VALUE, 1);
    floats("polygonOffset", GL_POLYGON_OFFSET_FACTOR, 1);
    floats("polygonOffsetUnits", GL_POLYGON_OFFSET_UNITS, 1);
    floats("lineWidth", GL_LINE_WIDTH, 1);
    floats("pointSize", GL_POINT_SIZE, 1);
    floats("fogColor", GL_FOG_COLOR, 4);
    floats("fogStart", GL_FOG_START, 1);
    floats("fogEnd", GL_FOG_END, 1);
    floats("fogDensity", GL_FOG_DENSITY, 1);
    floats("blendColor", kBLEND_COLOR, 4);
    ints("blendEquation", kBLEND_EQUATION, 1);
    ints("blendSrcRgb", kBLEND_SRC_RGB, 1);
    ints("blendDstRgb", kBLEND_DST_RGB, 1);
    ints("blendSrcAlpha", kBLEND_SRC_ALPHA, 1);
    ints("blendDstAlpha", kBLEND_DST_ALPHA, 1);
    ints("fogMode", GL_FOG_MODE, 1);
    ints("frontFace", GL_FRONT_FACE, 1);
    ints("stencilFail", GL_STENCIL_FAIL, 1);
    ints("stencilZFail", GL_STENCIL_PASS_DEPTH_FAIL, 1);
    ints("stencilClearValue", GL_STENCIL_CLEAR_VALUE, 1);
    ints("readBuffer", GL_READ_BUFFER, 1);
    ints("packAlignment", GL_PACK_ALIGNMENT, 1);
    ints("packRowLength", GL_PACK_ROW_LENGTH, 1);
    ints("unpackSkipRows", GL_UNPACK_SKIP_ROWS, 1);
    ints("unpackSkipPixels", GL_UNPACK_SKIP_PIXELS, 1);
    ints("maxViewportDims", GL_MAX_VIEWPORT_DIMS, 2);
    for (GLenum e : {GL_POLYGON_OFFSET_FILL, GL_NORMALIZE, GL_COLOR_MATERIAL, GL_DITHER, GL_LINE_SMOOTH, GL_POINT_SMOOTH,
                     GL_POLYGON_SMOOTH, GL_TEXTURE_1D, GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_CLIP_PLANE0, GL_LIGHT0,
                     GL_COLOR_LOGIC_OP, GL_AUTO_NORMAL, GL_INDEX_ARRAY, GL_EDGE_FLAG_ARRAY}) {
        snprintf(num, sizeof num, "enabled_0x%X", e);
        v.Bool(num, glIsEnabled(e) != 0);
    }
    if (c.fbo) {
        ints("drawFramebuffer", kDRAW_FRAMEBUFFER_BINDING, 1);
        ints("readFramebuffer", kREAD_FRAMEBUFFER_BINDING, 1);
        ints("renderbuffer", kRENDERBUFFER_BINDING, 1);
    }
    if (c.pbo) {
        ints("packBuffer", kPIXEL_PACK_BUFFER_BINDING, 1);
        ints("unpackBuffer", kPIXEL_UNPACK_BUFFER_BINDING, 1);
    }
    if (c.ActiveTexture) {
        GLint act = GetI(kACTIVE_TEXTURE), cact = c.ClientActiveTexture ? GetI(kCLIENT_ACTIVE_TEXTURE) : 0;
        std::string units = "[";
        int n = std::min(c.maxTexUnits, 8);
        for (int u = 0; u < n; ++u) {
            c.ActiveTexture(kTEXTURE0 + u);
            if (c.ClientActiveTexture && u < c.maxTexCoords) c.ClientActiveTexture(kTEXTURE0 + u);
            GLint env = 0;
            glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
            jsonmini::Obj o;
            o.Int("unit", u).Int("binding2D", GetI(GL_TEXTURE_BINDING_2D)).Bool("enabled2D", glIsEnabled(GL_TEXTURE_2D) != 0)
                .Int("envMode", env).Bool("texCoordArray", u < c.maxTexCoords && glIsEnabled(GL_TEXTURE_COORD_ARRAY));
            if (c.cube) o.Int("bindingCube", GetI(kTEXTURE_BINDING_CUBE));
            if (c.tex3d) o.Int("binding3D", GetI(kTEXTURE_BINDING_3D));
            units += (u ? "," : "") + o.End();
        }
        c.ActiveTexture(static_cast<GLenum>(act));
        if (c.ClientActiveTexture) c.ClientActiveTexture(static_cast<GLenum>(cact));
        v.Raw("textureUnits", units + "]");
    }
    render::gl::DrainErrors();
    jsonmini::Obj root;
    root.Str("format", "melange-capture").Int("version", 1).Str("which", which)
        .UInt("frame", events::FrameCount()).Raw("values", v.End());
    return root.End();
}

void ReadFrame(Job& j) {
    render::WindowSize(&j.frameW, &j.frameH);
    int w = j.frameW, h = j.frameH;
    if (w <= 0 || h <= 0) return;
    const render::gl::Caps& c = render::gl::Load();
    GLint readFbo = c.fbo ? GetI(kREAD_FRAMEBUFFER_BINDING) : 0, packBuf = c.pbo ? GetI(kPIXEL_PACK_BUFFER_BINDING) : 0;
    GLint readBuf = GetI(GL_READ_BUFFER), align = GetI(GL_PACK_ALIGNMENT), rowLen = GetI(GL_PACK_ROW_LENGTH),
          skipR = GetI(GL_PACK_SKIP_ROWS), skipP = GetI(GL_PACK_SKIP_PIXELS);
    if (readFbo && c.BindFramebuffer) c.BindFramebuffer(kREAD_FRAMEBUFFER, 0);
    if (packBuf && c.BindBuffer) c.BindBuffer(kPIXEL_PACK_BUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    std::vector<uint8_t> raw(static_cast<size_t>(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glPixelStorei(GL_PACK_ROW_LENGTH, rowLen);
    glPixelStorei(GL_PACK_SKIP_ROWS, skipR);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skipP);
    glReadBuffer(static_cast<GLenum>(readBuf));
    if (packBuf && c.BindBuffer) c.BindBuffer(kPIXEL_PACK_BUFFER, static_cast<GLuint>(packBuf));
    if (readFbo && c.BindFramebuffer) c.BindFramebuffer(kREAD_FRAMEBUFFER, static_cast<GLuint>(readFbo));
    j.frame.resize(raw.size());
    size_t row = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; ++y) memcpy(&j.frame[y * row], &raw[(h - 1 - y) * row], row);
    for (size_t i = 3; i < j.frame.size(); i += 4) j.frame[i] = 255;  // back-buffer alpha is not part of the picture
}

bool IsDepth(GLint f) {
    switch (f) {
        case 0x1902: case 0x81A5: case 0x81A6: case 0x81A7: case 0x88F0: case 0x84F9: case 0x8CAC: case 0x8CAD: return true;
        default: return false;
    }
}

void ReadTextures(Job& j) {
    std::set<uint32_t> names;
    size_t other = 0;
    for (size_t k = 0; k < j.recs.size(); ++k) {
        const hub::Rec& r = j.recs[k];
        const std::string& fn = j.fnName[r.fn];
        uint32_t target = 0, name = 0;
        if (fn == "glBindTexture" || fn == "glBindTextureEXT") target = r.a[0], name = r.a[1];
        else if (fn == "glBindMultiTextureEXT") target = r.a[1], name = r.a[2];
        if (!name) continue;
        if (target == GL_TEXTURE_2D) names.insert(name);
        else ++other;
    }
    if (other) j.notes.push_back(std::to_string(other) + " binds of non-2D textures were not read back");
    const render::gl::Caps& c = render::gl::Load();
    GLint act = c.ActiveTexture ? GetI(kACTIVE_TEXTURE) : kTEXTURE0;
    if (c.ActiveTexture) c.ActiveTexture(kTEXTURE0);
    GLint prev = GetI(GL_TEXTURE_BINDING_2D), packBuf = c.pbo ? GetI(kPIXEL_PACK_BUFFER_BINDING) : 0;
    GLint align = GetI(GL_PACK_ALIGNMENT), rowLen = GetI(GL_PACK_ROW_LENGTH), skipR = GetI(GL_PACK_SKIP_ROWS),
          skipP = GetI(GL_PACK_SKIP_PIXELS);
    if (packBuf && c.BindBuffer) c.BindBuffer(kPIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    render::gl::DrainErrors();
    uint64_t budget = static_cast<uint64_t>(j.opt.maxTextureMB) << 20, used = 0;
    for (uint32_t t : names) {
        TexOut o;
        o.gl = t;
        o.name = TextureName(t);
        if (!glIsTexture(t)) {
            o.skipped = "deleted";
            j.tex.push_back(std::move(o));
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, t);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &o.w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &o.h);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &o.ifmt);
        for (int l = 0; l < 16; ++l) {
            GLint lw = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, l, GL_TEXTURE_WIDTH, &lw);
            if (lw <= 0) break;
            o.levels = l + 1;
        }
        o.depth = IsDepth(o.ifmt);
        uint64_t bytes = static_cast<uint64_t>(std::max(o.w, 0)) * std::max(o.h, 0) * (o.depth ? 2 : 4);
        if (render::gl::DrainErrors() || !bytes) {
            o.skipped = "not a 2D texture";
        } else if (used + bytes > budget) {
            o.skipped = "maxTextureMB reached";
        } else {
            used += bytes;
            if (o.depth) {
                o.px16.resize(static_cast<size_t>(o.w) * o.h);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, o.px16.data());
            } else {
                o.px8.resize(static_cast<size_t>(o.w) * o.h * 4);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, o.px8.data());
            }
            if (render::gl::DrainErrors()) {
                o.skipped = "glGetTexImage failed";
                o.px8.clear();
                o.px16.clear();
            }
        }
        j.tex.push_back(std::move(o));
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glPixelStorei(GL_PACK_ROW_LENGTH, rowLen);
    glPixelStorei(GL_PACK_SKIP_ROWS, skipR);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skipP);
    if (packBuf && c.BindBuffer) c.BindBuffer(kPIXEL_PACK_BUFFER, static_cast<GLuint>(packBuf));
    if (c.ActiveTexture) c.ActiveTexture(static_cast<GLenum>(act));
    render::gl::DrainErrors();
}

void ReadPrograms(Job& j) {
    HMODULE cg = GetModuleHandleW(L"cg.dll"), cggl = GetModuleHandleW(L"cgGL.dll");
    auto getString = cg ? reinterpret_cast<const char*(__cdecl*)(uintptr_t, int)>(GetProcAddress(cg, "cgGetProgramString")) : nullptr;
    auto getError = cg ? reinterpret_cast<int(__cdecl*)()>(GetProcAddress(cg, "cgGetError")) : nullptr;
    auto getId = cggl ? reinterpret_cast<GLuint(__cdecl*)(uintptr_t)>(GetProcAddress(cggl, "cgGLGetProgramID")) : nullptr;
    struct Ctx {
        Job* j;
        decltype(getString) str;
        decltype(getError) err;
        decltype(getId) id;
    } ctx{&j, getString, getError, getId};
    engine::ForEachCgProg(
        [](const engine::CgProg& p, void* u) {
            auto* c = static_cast<Ctx*>(u);
            ProgOut o;
            char path[260] = {}, entry[128] = {};
            if (p.path) strncpy_s(path, p.path, _TRUNCATE);
            if (p.entry) strncpy_s(entry, p.entry, _TRUNCATE);
            const char* slash = std::max(strrchr(path, '/'), strrchr(path, '\\'));
            o.file = slash ? slash + 1 : path;
            o.source = path;
            o.entry = entry;
            o.stage = p.type;
            o.failed = p.failed;
            o.binds = p.binds;
            o.cgProgram = static_cast<uint32_t>(p.program);
            if (p.program && !p.failed) {
                // CG_COMPILED_PROGRAM
                if (c->str) {
                    const char* s = c->str(p.program, 0x100A);
                    if (s) o.asmText = s;
                    if (c->err) c->err();
                }
                if (c->id) {
                    o.arbName = c->id(p.program);
                    if (c->err) c->err();
                }
            }
            c->j->progs.push_back(std::move(o));
        },
        &ctx);
    std::vector<shaders::ProgramInfo> infos(512);
    infos.resize(shaders::ListPrograms(infos.data(), infos.size()));
    for (ProgOut& o : j.progs)
        for (const shaders::ProgramInfo& i : infos)
            if (i.file && i.entry && o.file == i.file && o.entry == i.entry) {
                o.owner = i.owner ? i.owner : "";
                o.overridden = i.overridden;
                o.glsl = i.glsl;
            }
    render::gl::DrainErrors();
}

void Write(Job* jp) {
    std::unique_ptr<Job> j(jp);
    McapResult r;
    std::string err;
    if (!WriteMcap(*j, j->path, &r, &err)) {
        SetState(St::Failed, err);
        LOG_ERROR("[mirage] capture failed: %s", err.c_str());
        return;
    }
    {
        std::lock_guard lk(g_mx);
        g_path = j->path;
    }
    SetState(St::Done);
    std::string p = game::Narrow(j->path);
    LOG_INFO("[mirage] capture written: %s (%llu calls, %llu draws, %llu textures, %llu programs, %llu dropped, "
             "readbackMs=%.1f)", p.c_str(), r.calls, r.draws, r.textures, r.programs,
             static_cast<unsigned long long>(j->dropped), j->readbackMs);
    jlog::Rec("mirage", jlog::Level::Info, "capture").Str("path", p).Uint("calls", r.calls).Uint("draws", r.draws)
        .Uint("textures", r.textures).Uint("programs", r.programs).Uint("programsWithAsm", r.programsWithAsm)
        .Uint("dropped", j->dropped).Uint("firstFrame", j->firstFrame).Uint("lastFrame", j->lastFrame)
        .Float("readbackMs", j->readbackMs);
}

std::wstring Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[64];
    swprintf(b, 64, L"%04u-%02u-%02u_%02u-%02u-%02u_f%llu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             static_cast<unsigned long long>(events::FrameCount()));
    return b;
}

void Finish() {
    Job& j = *g_job;
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    Drain();
    g_recording = false;
    hub::SetMode(g_savedMode);
    j.lastFrame = events::FrameCount() - 1;
    j.end = StateJson("end");
    if (j.opt.frameImage) ReadFrame(j);
    if (j.opt.shaders && game::IsKnownBuild()) ReadPrograms(j);
    j.fnName.resize(hub::Count());
    j.fnSrc.resize(hub::Count());
    for (int i = 0; i < hub::Count(); ++i) {
        j.fnName[i] = hub::Name(i);
        j.fnSrc[i] = hub::Source(i);
    }
    if (j.opt.textures) ReadTextures(j);
    RemoveStageMarkers();
    if (!j.opt.bufferSizes)
        for (auto it = j.pay.begin(); it != j.pay.end();)
            it = it->second.hashOnly ? j.pay.erase(it) : std::next(it);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    j.readbackMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(fq.QuadPart);
    Job* done = g_job;
    g_job = nullptr;
    SetState(St::Writing);
    RunAsync([done] { Write(done); });
}
}  // namespace

std::wstring CaptureDir() {
    std::string d = config::GetString("MirageTrace", "CaptureDir", "");
    return d.empty() ? MelangeDocsDir() + L"\\captures" : Widen(d);
}

bool CaptureRequest(const CaptureOptions& opt, std::string* error) {
    auto fail = [&](const char* e) {
        if (error) *error = e;
        return false;
    };
    if (!hub::Installed()) return fail("needs the GL hub ([MirageTrace] Mode=count or log at start)");
    {
        std::lock_guard lk(g_mx);
        if (g_state == St::Armed || g_state == St::Recording || g_state == St::Writing) return fail("a capture is running");
        g_opt = opt;
        g_opt.frames = std::clamp<uint32_t>(opt.frames, 1, 8);
        g_state = St::Armed;
        g_error.clear();
    }
    AddStageMarkers();
    LOG_INFO("[mirage] capture armed: %u frame(s), textures=%d shaders=%d", std::clamp<uint32_t>(opt.frames, 1, 8), opt.textures,
             opt.shaders);
    return true;
}

bool CaptureBusy() {
    std::lock_guard lk(g_mx);
    return g_state == St::Armed || g_state == St::Recording;
}

gltrace::CaptureState CaptureState(std::wstring* path, std::string* error) {
    std::lock_guard lk(g_mx);
    if (path) *path = g_path;
    if (error) *error = g_error;
    return g_state;
}

void CaptureOnFrame() {
    St st;
    {
        std::lock_guard lk(g_mx);
        st = g_state;
    }
    if (st == St::Armed) {
        std::wstring dir = CaptureDir();
        if (!EnsureDir(dir)) {
            RemoveStageMarkers();
            SetState(St::Failed, "cannot create the capture folder");
            return;
        }
        InstallPayloadTaps();
        g_savedMode = hub::GetMode();
        hub::SetMode(hub::Mode::Log);
        if (hub::GetMode() != hub::Mode::Log) {
            RemoveStageMarkers();
            SetState(St::Failed, "log mode unavailable (ring allocation failed)");
            return;
        }
        g_job = new Job;
        {
            std::lock_guard lk(g_mx);
            g_job->opt = g_opt;
        }
        g_job->path = dir + L"\\" + Stamp() + L".mcap";
        g_job->scene = Scene();
        overlay::GlInfo gi = overlay::Gl();
        g_job->glVendor = gi.vendor;
        g_job->glRenderer = gi.renderer;
        g_job->glVersion = gi.version;
        g_job->melangeVersion = MELANGE_VERSION;
        g_job->exeSha256 = game::Exe().sha256;
        g_job->exeBuild = game::Exe().build;
        render::WindowSize(&g_job->winW, &g_job->winH);
        g_job->begin = StateJson("begin");
        g_ringCap = 1u << std::clamp(config::GetInt("MirageTrace", "RingLog2", 17), 10, 22);
        g_readPos = hub::RingPos();
        g_job->firstFrame = events::FrameCount();
        g_framesLeft = g_job->opt.frames;
        g_recording = true;
        SetState(St::Recording);
        return;
    }
    if (st != St::Recording || !g_job) return;
    if (--g_framesLeft > 0) {
        Drain();
        return;
    }
    Finish();
}
}  // namespace melange::mirage::trace
