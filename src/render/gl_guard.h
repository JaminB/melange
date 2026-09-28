#pragma once
// GL state save / neutralise / restore around the overlay draw (docs/m0-design.md section 3.A "GL guard").
// Self-contained (windows.h + opengl32 only) so the offline test runner can drive it against a real context.
// Everything here must be called on the thread that owns the current GL context.
#include <cstdint>
#include <string>

namespace melange::render::gl {
// Per-context capabilities and extension entry points (resolved with wglGetProcAddress).
struct Caps {
    bool loaded = false;
    void* context = nullptr;  // HGLRC these were resolved for
    int major = 1, minor = 1;
    bool arbVp = false, arbFp = false;  // GL_ARB_vertex_program / GL_ARB_fragment_program
    bool glsl = false;                  // glUseProgram + GL_CURRENT_PROGRAM (GL 2.0)
    bool vbo = false, pbo = false, fbo = false;
    bool tex3d = false, cube = false, rect = false, multisample = false, srgb = false;
    bool secondaryColor = false, fogCoord = false;
    int maxTexUnits = 1, maxTexCoords = 1, maxVertexAttribs = 0;
    void(__stdcall* ActiveTexture)(unsigned) = nullptr;
    void(__stdcall* ClientActiveTexture)(unsigned) = nullptr;
    void(__stdcall* UseProgram)(unsigned) = nullptr;
    void(__stdcall* BindBuffer)(unsigned, unsigned) = nullptr;
    void(__stdcall* BlendEquation)(unsigned) = nullptr;
    void(__stdcall* DisableVertexAttribArray)(unsigned) = nullptr;
    void(__stdcall* GetProgramivARB)(unsigned, unsigned, int*) = nullptr;
};

// Resolves (once per context) and returns the caps of the current context; `loaded` is false without one.
const Caps& Load();
// Forget the cached caps (context change).
void Reset();

// Drains glGetError. Returns how many errors were pending; the first one goes to *first.
int DrainErrors(unsigned* first = nullptr);

// About 50 read-back values that the overlay must leave untouched (used by [Overlay] VerifyState=1 and the
// offline test). Reading drains glGetError before and after (queries of unsupported enums are skipped).
struct Snapshot {
    static constexpr int kMax = 80;
    int n = 0;
    const char* name[kMax] = {};
    double v[kMax] = {};
    int errorsBefore = 0;  // pending game errors drained before the read
    void Add(const char* k, double value) {
        if (n < kMax) {
            name[n] = k;
            v[n++] = value;
        }
    }
};
Snapshot Read();
// Number of differing values; a readable list of the first few goes to *out.
int Diff(const Snapshot& before, const Snapshot& after, std::string* out);

// RAII guard: the constructor saves all state (attrib + client attrib stacks, the three matrix stacks, buffer and
// program bindings) and neutralises the game's leftovers; the destructor restores everything.
// If a framebuffer object is bound (never seen, docs/m0-design.md U1) nothing is touched and Ok() is false.
class Guard {
public:
    Guard();
    ~Guard();  // calls Restore() if it has not run yet
    void Restore();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    bool Ok() const { return ok_; }
    int Fbo() const { return fbo_; }            // non-zero: the frame was skipped
    int SetupErrors() const { return setupErrors_; }  // GL errors raised by the neutralising calls (0 expected)
    int RestoreErrors() const { return restoreErrors_; }  // GL errors raised while drawing/restoring (0 expected)

private:
    bool ok_ = false, pushed_ = false;
    int fbo_ = 0, setupErrors_ = 0, restoreErrors_ = 0;
    int activeTex_ = 0, clientActiveTex_ = 0, matrixMode_ = 0, program_ = 0, arrayBuf_ = 0, elemBuf_ = 0,
        unpackBuf_ = 0;
    // Stack depths right after this Guard's own push (see gl_guard.cpp). ImGui's GL2 backend does its own
    // glPushAttrib + matrix pushes around the draw; if it faults mid-draw those pushes are never popped, so a
    // single unconditional glPopAttrib()/glPopMatrix() in Restore() would remove ImGui's level instead of ours
    // and leave the guard's own push permanently on the stack. Restore() instead pops down to these depths.
    int attribDepth_ = 0, clientAttribDepth_ = 0, texMatrixDepth_ = 0, projMatrixDepth_ = 0, mvMatrixDepth_ = 0;
};
}  // namespace melange::render::gl
