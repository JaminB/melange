// GL guard for the overlay: see gl_guard.h and docs/m0-design.md section 3.A.
//
// Recipe (the save/restore part is the probe's, verified in every scene [V]; the neutralising part is [I]):
//   1. push GL_ALL_ATTRIB_BITS, GL_CLIENT_ALL_ATTRIB_BITS and the texture (unit 0), projection and modelview
//      matrix stacks;
//   2. disable the ARB vertex/fragment programs with glDisable (never cgGLDisableProfile: the Cg runtime caches
//      the profile state at mgr+0x398);
//   3. glUseProgram(0) only when a GLSL program is current (it never was at Present [V]);
//   4. disable stencil, depth, alpha test, cull, lighting, fog (+ everything else fixed-function that would tint
//      or clip ImGui's triangles: texture targets on every unit, texgen, clip planes, logic op, color sum...);
//   5. texture unit 0 active, client arrays and buffer objects (VBO/IBO/PBO) out of the way: ImGui's GL2
//      backend draws from client memory, so a bound VBO would turn its pointers into offsets;
//   6. a bound FBO means "skip the frame" (never rebind);
//   7. ImGui draws with its own viewport and ortho projection;
//   8. pop everything in reverse order, then restore what the attrib stacks do not cover (buffer bindings,
//      program) explicitly.
#include "render/gl_guard.h"

#include <windows.h>
#include <GL/gl.h>

#include <cstdio>
#include <cstring>

namespace wf::render::gl {
namespace {
// Enums beyond the GL 1.1 header.
constexpr GLenum kTEXTURE0 = 0x84C0, kACTIVE_TEXTURE = 0x84E0, kCLIENT_ACTIVE_TEXTURE = 0x84E1,
                 kMAX_TEXTURE_UNITS = 0x84E2, kMAX_TEXTURE_COORDS = 0x8871, kMAX_VERTEX_ATTRIBS = 0x8869,
                 kVERTEX_PROGRAM_ARB = 0x8620, kFRAGMENT_PROGRAM_ARB = 0x8804, kPROGRAM_BINDING_ARB = 0x8677,
                 kCURRENT_PROGRAM = 0x8B8D, kARRAY_BUFFER = 0x8892, kELEMENT_ARRAY_BUFFER = 0x8893,
                 kARRAY_BUFFER_BINDING = 0x8894, kELEMENT_ARRAY_BUFFER_BINDING = 0x8895,
                 kPIXEL_UNPACK_BUFFER = 0x88EC, kPIXEL_UNPACK_BUFFER_BINDING = 0x88EF, kFRAMEBUFFER_BINDING = 0x8CA6,
                 kTEXTURE_3D = 0x806F, kTEXTURE_CUBE_MAP = 0x8513, kTEXTURE_RECTANGLE = 0x84F5,
                 kMULTISAMPLE = 0x809D, kSAMPLE_ALPHA_TO_COVERAGE = 0x809E, kFRAMEBUFFER_SRGB = 0x8DB9,
                 kCOLOR_SUM = 0x8458, kSECONDARY_COLOR_ARRAY = 0x845E, kFOG_COORD_ARRAY = 0x8457,
                 kFUNC_ADD = 0x8006, kUNPACK_IMAGE_HEIGHT = 0x806E, kUNPACK_SKIP_IMAGES = 0x806D,
                 kCLIENT_ATTRIB_STACK_DEPTH = 0x0BB1;

Caps g_caps;

template <class T>
void Proc(T& fn, std::initializer_list<const char*> names) {
    fn = nullptr;
    for (const char* n : names) {
        PROC p = wglGetProcAddress(n);
        auto v = reinterpret_cast<intptr_t>(p);
        if (p && v != 1 && v != 2 && v != 3 && v != -1) {
            fn = reinterpret_cast<T>(p);
            return;
        }
    }
}

bool HasExt(const char* exts, const char* name) {
    if (!exts) return false;
    size_t len = strlen(name);
    for (const char* p = exts; (p = strstr(p, name)) != nullptr; p += len) {
        bool startOk = p == exts || p[-1] == ' ';
        bool endOk = p[len] == ' ' || p[len] == 0;
        if (startOk && endOk) return true;
    }
    return false;
}

GLint GetI(GLenum e) {
    GLint v[4] = {0, 0, 0, 0};
    glGetIntegerv(e, v);
    return v[0];
}

uint32_t Fnv(const void* data, size_t n) {
    uint32_t h = 2166136261u;
    auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
}  // namespace

const Caps& Load() {
    HGLRC ctx = wglGetCurrentContext();
    if (!ctx) {
        static Caps none;
        return none;
    }
    if (g_caps.loaded && g_caps.context == ctx) return g_caps;
    Caps c;
    c.context = ctx;
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (ver) sscanf(ver, "%d.%d", &c.major, &c.minor);
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    auto atLeast = [&](int ma, int mi) { return c.major > ma || (c.major == ma && c.minor >= mi); };

    Proc(c.ActiveTexture, {"glActiveTexture", "glActiveTextureARB"});
    Proc(c.ClientActiveTexture, {"glClientActiveTexture", "glClientActiveTextureARB"});
    if (atLeast(2, 0)) Proc(c.UseProgram, {"glUseProgram"});
    if (atLeast(1, 5) || HasExt(ext, "GL_ARB_vertex_buffer_object")) Proc(c.BindBuffer, {"glBindBuffer", "glBindBufferARB"});
    Proc(c.BlendEquation, {"glBlendEquation", "glBlendEquationEXT"});
    Proc(c.DisableVertexAttribArray, {"glDisableVertexAttribArray", "glDisableVertexAttribArrayARB"});
    Proc(c.GetProgramivARB, {"glGetProgramivARB"});

    c.arbVp = HasExt(ext, "GL_ARB_vertex_program");
    c.arbFp = HasExt(ext, "GL_ARB_fragment_program");
    c.glsl = c.UseProgram != nullptr;
    c.vbo = c.BindBuffer != nullptr;
    c.pbo = c.vbo && (atLeast(2, 1) || HasExt(ext, "GL_ARB_pixel_buffer_object") || HasExt(ext, "GL_EXT_pixel_buffer_object"));
    c.fbo = atLeast(3, 0) || HasExt(ext, "GL_EXT_framebuffer_object") || HasExt(ext, "GL_ARB_framebuffer_object");
    c.tex3d = atLeast(1, 2) || HasExt(ext, "GL_EXT_texture3D");
    c.cube = atLeast(1, 3) || HasExt(ext, "GL_ARB_texture_cube_map") || HasExt(ext, "GL_EXT_texture_cube_map");
    c.rect = atLeast(3, 1) || HasExt(ext, "GL_ARB_texture_rectangle") || HasExt(ext, "GL_EXT_texture_rectangle") ||
             HasExt(ext, "GL_NV_texture_rectangle");
    c.multisample = atLeast(1, 3) || HasExt(ext, "GL_ARB_multisample");
    c.srgb = atLeast(3, 0) || HasExt(ext, "GL_ARB_framebuffer_sRGB") || HasExt(ext, "GL_EXT_framebuffer_sRGB");
    c.secondaryColor = atLeast(1, 4) || HasExt(ext, "GL_EXT_secondary_color");
    c.fogCoord = atLeast(1, 4) || HasExt(ext, "GL_EXT_fog_coord");
    if (c.ActiveTexture) {
        c.maxTexUnits = GetI(kMAX_TEXTURE_UNITS);
        c.maxTexCoords = (atLeast(2, 0) || c.arbFp) ? GetI(kMAX_TEXTURE_COORDS) : c.maxTexUnits;
    }
    if (c.maxTexUnits < 1) c.maxTexUnits = 1;
    if (c.maxTexUnits > 32) c.maxTexUnits = 32;
    if (c.maxTexCoords < 1) c.maxTexCoords = 1;
    if (c.maxTexCoords > 32) c.maxTexCoords = 32;
    if (c.DisableVertexAttribArray && (c.glsl || c.arbVp)) {
        c.maxVertexAttribs = GetI(kMAX_VERTEX_ATTRIBS);
        if (c.maxVertexAttribs > 32) c.maxVertexAttribs = 32;
        if (c.maxVertexAttribs < 0) c.maxVertexAttribs = 0;
    }
    DrainErrors();
    c.loaded = true;
    g_caps = c;
    return g_caps;
}

void Reset() { g_caps = Caps{}; }

int DrainErrors(unsigned* first) {
    int n = 0;
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR && n < 64; ++n)
        if (n == 0 && first) *first = e;
    return n;
}

Snapshot Read() {
    Snapshot s;
    const Caps& c = Load();
    s.errorsBefore = DrainErrors();
    if (!c.loaded) return s;
    auto en = [&](const char* k, GLenum e) { s.Add(k, glIsEnabled(e) ? 1 : 0); };
    auto gi = [&](const char* k, GLenum e) { s.Add(k, GetI(e)); };
    en("blend", GL_BLEND);
    gi("blendSrc", GL_BLEND_SRC);
    gi("blendDst", GL_BLEND_DST);
    en("depthTest", GL_DEPTH_TEST);
    gi("depthFunc", GL_DEPTH_FUNC);
    gi("depthMask", GL_DEPTH_WRITEMASK);
    en("stencilTest", GL_STENCIL_TEST);
    gi("stencilFunc", GL_STENCIL_FUNC);
    gi("stencilRef", GL_STENCIL_REF);
    gi("stencilValueMask", GL_STENCIL_VALUE_MASK);
    gi("stencilWriteMask", GL_STENCIL_WRITEMASK);
    gi("stencilZPass", GL_STENCIL_PASS_DEPTH_PASS);
    en("alphaTest", GL_ALPHA_TEST);
    gi("alphaFunc", GL_ALPHA_TEST_FUNC);
    {
        GLfloat r = 0;
        glGetFloatv(GL_ALPHA_TEST_REF, &r);
        s.Add("alphaRef", r);
    }
    en("cullFace", GL_CULL_FACE);
    gi("cullMode", GL_CULL_FACE_MODE);
    en("scissorTest", GL_SCISSOR_TEST);
    en("lighting", GL_LIGHTING);
    en("fog", GL_FOG);
    en("texture2D", GL_TEXTURE_2D);
    en("vertexArray", GL_VERTEX_ARRAY);
    en("texCoordArray", GL_TEXTURE_COORD_ARRAY);
    en("colorArray", GL_COLOR_ARRAY);
    en("normalArray", GL_NORMAL_ARRAY);
    if (c.multisample) en("multisample", kMULTISAMPLE);
    if (c.arbVp) en("arbVertexProgram", kVERTEX_PROGRAM_ARB);
    if (c.arbFp) en("arbFragmentProgram", kFRAGMENT_PROGRAM_ARB);
    if (c.GetProgramivARB) {
        GLint b = 0;
        if (c.arbVp) {
            c.GetProgramivARB(kVERTEX_PROGRAM_ARB, kPROGRAM_BINDING_ARB, &b);
            s.Add("arbVpBinding", b);
        }
        if (c.arbFp) {
            c.GetProgramivARB(kFRAGMENT_PROGRAM_ARB, kPROGRAM_BINDING_ARB, &b);
            s.Add("arbFpBinding", b);
        }
    }
    if (c.glsl) gi("glslProgram", kCURRENT_PROGRAM);
    if (c.vbo) {
        gi("arrayBuffer", kARRAY_BUFFER_BINDING);
        gi("elementBuffer", kELEMENT_ARRAY_BUFFER_BINDING);
    }
    if (c.pbo) gi("unpackBuffer", kPIXEL_UNPACK_BUFFER_BINDING);
    if (c.fbo) gi("framebuffer", kFRAMEBUFFER_BINDING);
    {
        GLint vp[4] = {};
        glGetIntegerv(GL_VIEWPORT, vp);
        s.Add("viewportX", vp[0]);
        s.Add("viewportY", vp[1]);
        s.Add("viewportW", vp[2]);
        s.Add("viewportH", vp[3]);
        GLint sc[4] = {};
        glGetIntegerv(GL_SCISSOR_BOX, sc);
        s.Add("scissorX", sc[0]);
        s.Add("scissorY", sc[1]);
        s.Add("scissorW", sc[2]);
        s.Add("scissorH", sc[3]);
    }
    gi("matrixMode", GL_MATRIX_MODE);
    if (c.ActiveTexture) {
        gi("activeTexture", kACTIVE_TEXTURE);
        gi("clientActiveTexture", kCLIENT_ACTIVE_TEXTURE);
    }
    gi("textureBinding2D", GL_TEXTURE_BINDING_2D);
    {
        GLint te = 0;
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &te);
        s.Add("texEnvMode", te);
    }
    {
        GLboolean cm[4] = {};
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        s.Add("colorMask", cm[0] | cm[1] << 1 | cm[2] << 2 | cm[3] << 3);
        GLint pm[2] = {};
        glGetIntegerv(GL_POLYGON_MODE, pm);
        s.Add("polygonModeFront", pm[0]);
        s.Add("polygonModeBack", pm[1]);
    }
    gi("shadeModel", GL_SHADE_MODEL);
    gi("unpackAlignment", GL_UNPACK_ALIGNMENT);
    gi("unpackRowLength", GL_UNPACK_ROW_LENGTH);
    gi("drawBuffer", GL_DRAW_BUFFER);
    gi("attribStackDepth", GL_ATTRIB_STACK_DEPTH);
    gi("clientAttribStackDepth", kCLIENT_ATTRIB_STACK_DEPTH);
    gi("modelviewStackDepth", GL_MODELVIEW_STACK_DEPTH);
    gi("projectionStackDepth", GL_PROJECTION_STACK_DEPTH);
    gi("textureStackDepth", GL_TEXTURE_STACK_DEPTH);
    {
        GLfloat m[16] = {};
        glGetFloatv(GL_PROJECTION_MATRIX, m);
        s.Add("projectionMatrixHash", Fnv(m, sizeof(m)));
        glGetFloatv(GL_MODELVIEW_MATRIX, m);
        s.Add("modelviewMatrixHash", Fnv(m, sizeof(m)));
        glGetFloatv(GL_TEXTURE_MATRIX, m);
        s.Add("textureMatrixHash", Fnv(m, sizeof(m)));
        GLfloat col[4] = {};
        glGetFloatv(GL_CURRENT_COLOR, col);
        s.Add("currentColorHash", Fnv(col, sizeof(col)));
    }
    DrainErrors();  // unsupported queries on exotic drivers must not leak INVALID_ENUM to the game
    return s;
}

int Diff(const Snapshot& a, const Snapshot& b, std::string* out) {
    int diffs = 0;
    int n = a.n < b.n ? a.n : b.n;
    for (int i = 0; i < n; ++i) {
        if (a.v[i] == b.v[i]) continue;
        if (out && diffs < 8) {
            char line[128];
            snprintf(line, sizeof(line), "%s%s %.6g -> %.6g", out->empty() ? "" : ", ", a.name[i], a.v[i], b.v[i]);
            *out += line;
        }
        ++diffs;
    }
    if (a.n != b.n) {
        ++diffs;
        if (out) *out += " (value count differs)";
    }
    return diffs;
}

Guard::Guard() {
    const Caps& c = Load();
    if (!c.loaded) return;
    DrainErrors();  // do not attribute earlier (game) errors to us
    if (c.fbo) {
        fbo_ = GetI(kFRAMEBUFFER_BINDING);
        if (fbo_ != 0) return;  // 6. skip the frame, never rebind
    }
    if (c.ActiveTexture) {
        activeTex_ = GetI(kACTIVE_TEXTURE);
        clientActiveTex_ = c.ClientActiveTexture ? GetI(kCLIENT_ACTIVE_TEXTURE) : static_cast<int>(kTEXTURE0);
    }
    matrixMode_ = GetI(GL_MATRIX_MODE);
    if (c.glsl) program_ = GetI(kCURRENT_PROGRAM);
    if (c.vbo) {
        arrayBuf_ = GetI(kARRAY_BUFFER_BINDING);
        elemBuf_ = GetI(kELEMENT_ARRAY_BUFFER_BINDING);
    }
    if (c.pbo) unpackBuf_ = GetI(kPIXEL_UNPACK_BUFFER_BINDING);

    // 1. save
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    if (c.ActiveTexture) c.ActiveTexture(kTEXTURE0);
    if (c.ClientActiveTexture) c.ClientActiveTexture(kTEXTURE0);
    glMatrixMode(GL_TEXTURE);  // texture matrix of unit 0 (ImGui does not reset it)
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    pushed_ = true;

    // 2. ARB programs (glDisable only)
    if (c.arbVp) glDisable(kVERTEX_PROGRAM_ARB);
    if (c.arbFp) glDisable(kFRAGMENT_PROGRAM_ARB);
    // 3. GLSL program only when one is current
    if (c.glsl && program_ != 0) c.UseProgram(0);
    // 4. fixed-function state the game leaves on at Present
    for (GLenum e : {GL_STENCIL_TEST, GL_DEPTH_TEST, GL_ALPHA_TEST, GL_CULL_FACE, GL_LIGHTING, GL_FOG, GL_SCISSOR_TEST,
                     GL_COLOR_LOGIC_OP, GL_POLYGON_OFFSET_FILL, GL_POLYGON_STIPPLE, GL_COLOR_MATERIAL, GL_DITHER,
                     GL_CLIP_PLANE0, GL_CLIP_PLANE1, GL_CLIP_PLANE2, GL_CLIP_PLANE3, GL_CLIP_PLANE4, GL_CLIP_PLANE5})
        glDisable(e);
    if (c.multisample) {
        glDisable(kMULTISAMPLE);
        glDisable(kSAMPLE_ALPHA_TO_COVERAGE);
    }
    if (c.srgb) glDisable(kFRAMEBUFFER_SRGB);
    if (c.secondaryColor) glDisable(kCOLOR_SUM);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (c.BlendEquation) c.BlendEquation(kFUNC_ADD);
    // 5. texture units: only 2D on unit 0 (ImGui enables it), nothing on the others
    for (int u = c.maxTexUnits - 1; u >= 0; --u) {
        if (c.ActiveTexture) c.ActiveTexture(kTEXTURE0 + static_cast<GLenum>(u));
        else if (u != 0) continue;
        glDisable(GL_TEXTURE_1D);
        if (u != 0) glDisable(GL_TEXTURE_2D);
        if (c.tex3d) glDisable(kTEXTURE_3D);
        if (c.cube) glDisable(kTEXTURE_CUBE_MAP);
        if (c.rect) glDisable(kTEXTURE_RECTANGLE);
        for (GLenum g : {GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q}) glDisable(g);
    }
    // client arrays: ImGui enables vertex/texcoord(unit 0)/color and disables normal itself
    if (c.ClientActiveTexture) {
        for (int u = c.maxTexCoords - 1; u >= 1; --u) {
            c.ClientActiveTexture(kTEXTURE0 + static_cast<GLenum>(u));
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        c.ClientActiveTexture(kTEXTURE0);
    }
    glDisableClientState(GL_INDEX_ARRAY);
    glDisableClientState(GL_EDGE_FLAG_ARRAY);
    if (c.secondaryColor) glDisableClientState(kSECONDARY_COLOR_ARRAY);
    if (c.fogCoord) glDisableClientState(kFOG_COORD_ARRAY);
    // generic attribute 0 aliases the vertex position on some drivers
    for (int i = 0; i < c.maxVertexAttribs; ++i) c.DisableVertexAttribArray(static_cast<GLuint>(i));
    if (c.vbo) {
        c.BindBuffer(kARRAY_BUFFER, 0);
        c.BindBuffer(kELEMENT_ARRAY_BUFFER, 0);
    }
    if (c.pbo) c.BindBuffer(kPIXEL_UNPACK_BUFFER, 0);
    // pixel-store (the ImGui font upload reads client memory); saved by the client attrib push
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    if (c.tex3d) {
        glPixelStorei(kUNPACK_IMAGE_HEIGHT, 0);
        glPixelStorei(kUNPACK_SKIP_IMAGES, 0);
    }
    glColor4f(1.f, 1.f, 1.f, 1.f);
    setupErrors_ = DrainErrors();
    ok_ = true;
}

Guard::~Guard() { Restore(); }

void Guard::Restore() {
    if (!pushed_) return;
    pushed_ = false;
    const Caps& c = Load();
    // 8. pop in reverse order
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    if (c.ActiveTexture) c.ActiveTexture(kTEXTURE0);
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    // not (reliably) covered by the attrib stacks
    if (c.vbo) {
        c.BindBuffer(kARRAY_BUFFER, static_cast<GLuint>(arrayBuf_));
        c.BindBuffer(kELEMENT_ARRAY_BUFFER, static_cast<GLuint>(elemBuf_));
    }
    if (c.pbo) c.BindBuffer(kPIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuf_));
    if (c.glsl && program_ != 0) c.UseProgram(static_cast<GLuint>(program_));
    if (c.ActiveTexture) c.ActiveTexture(static_cast<GLenum>(activeTex_));
    if (c.ClientActiveTexture) c.ClientActiveTexture(static_cast<GLenum>(clientActiveTex_));
    glMatrixMode(static_cast<GLenum>(matrixMode_));
    restoreErrors_ = DrainErrors();
}
}  // namespace wf::render::gl
