#pragma once
// GL state save / neutralise / restore around the overlay draw. Call only on the thread owning the GL context.
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
    void(__stdcall* BindFramebuffer)(unsigned, unsigned) = nullptr;
};

// Resolves (once per context) and returns the caps of the current context; `loaded` is false without one.
const Caps& Load();
// Forget the cached caps (context change).
void Reset();

// Drains glGetError. Returns how many errors were pending; the first one goes to *first.
int DrainErrors(unsigned* first = nullptr);

// Read-back values the overlay must leave untouched ([Overlay] VerifyState=1). Drains glGetError around the read.
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

// RAII: saves all state and neutralises the game's leftovers; the destructor restores everything.
// Overlay: if a framebuffer object is bound nothing is touched and Ok() is false.
// Stage: works inside the engine's scene FBO (restored on exit), keeps depth test, also disables client arrays.
enum class Flavor { Overlay, Stage };
class Guard {
public:
    explicit Guard(Flavor flavor = Flavor::Overlay);
    ~Guard();
    void Restore();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    bool Ok() const { return ok_; }
    int Fbo() const { return fbo_; }            // non-zero: the frame was skipped
    int SetupErrors() const { return setupErrors_; }  // GL errors raised by the neutralising calls (0 expected)
    int RestoreErrors() const { return restoreErrors_; }  // GL errors raised while drawing/restoring (0 expected)

private:
    Flavor flavor_;
    bool ok_ = false, pushed_ = false;
    int fbo_ = 0, setupErrors_ = 0, restoreErrors_ = 0;
    int activeTex_ = 0, clientActiveTex_ = 0, matrixMode_ = 0, program_ = 0, arrayBuf_ = 0, elemBuf_ = 0,
        unpackBuf_ = 0;
    // Stack depths right after our own push. Restore() pops down to these, so pushes ImGui left behind after a
    // mid-draw fault are removed too.
    int attribDepth_ = 0, clientAttribDepth_ = 0, texMatrixDepth_ = 0, projMatrixDepth_ = 0, mvMatrixDepth_ = 0;
};
}  // namespace melange::render::gl
