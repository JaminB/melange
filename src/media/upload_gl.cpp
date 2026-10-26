// A player's video texture and its PBO upload (upload_gl.h).
#include "media/upload_gl.h"

#include <windows.h>
#include <GL/gl.h>

#include <cstring>

#include "media/shm_logic.h"
#include "melange/render.h"
#include "render/gl_guard.h"

namespace melange::media::gl {
namespace {
constexpr GLenum kPixelUnpackBuffer = 0x88EC, kStreamDraw = 0x88E0, kWriteOnly = 0x88B9, kBgra = 0x80E1,
                 kUint8888Rev = 0x8367, kClampToEdge = 0x812F, kRgb8 = 0x8051;
constexpr GLsizei kTexW = shm::kMaxW, kTexH = shm::kMaxH;

struct Pbo {
    void* context = nullptr;
    bool ok = false;
    void(__stdcall* GenBuffers)(GLsizei, GLuint*) = nullptr;
    void(__stdcall* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void(__stdcall* BindBuffer)(GLenum, GLuint) = nullptr;
    void(__stdcall* BufferData)(GLenum, ptrdiff_t, const void*, GLenum) = nullptr;
    void*(__stdcall* MapBuffer)(GLenum, GLenum) = nullptr;
    GLboolean(__stdcall* UnmapBuffer)(GLenum) = nullptr;
};

template <class F>
void Resolve(F* f, const char* name, const char* arb) {
    *f = reinterpret_cast<F>(wglGetProcAddress(name));
    if (!*f) *f = reinterpret_cast<F>(wglGetProcAddress(arb));
}

const Pbo& Funcs() {
    static Pbo p;
    void* ctx = wglGetCurrentContext();
    if (p.context == ctx) return p;
    p = Pbo();
    p.context = ctx;
    const render::gl::Caps& caps = render::gl::Load();
    if (!caps.loaded || !caps.pbo) return p;
    Resolve(&p.GenBuffers, "glGenBuffers", "glGenBuffersARB");
    Resolve(&p.DeleteBuffers, "glDeleteBuffers", "glDeleteBuffersARB");
    Resolve(&p.BindBuffer, "glBindBuffer", "glBindBufferARB");
    Resolve(&p.BufferData, "glBufferData", "glBufferDataARB");
    Resolve(&p.MapBuffer, "glMapBuffer", "glMapBufferARB");
    Resolve(&p.UnmapBuffer, "glUnmapBuffer", "glUnmapBufferARB");
    p.ok = p.GenBuffers && p.DeleteBuffers && p.BindBuffer && p.BufferData && p.MapBuffer && p.UnmapBuffer;
    return p;
}
}  // namespace

bool Uploader::Upload(const uint8_t* data, uint32_t w, uint32_t h) {
    if (!data || w < 1 || h < 1 || w > shm::kMaxW || h > shm::kMaxH) return false;
    void* ctx = wglGetCurrentContext();
    if (!ctx) return false;
    if (context_ && context_ != ctx) {  // the game recreated its context: the old names mean nothing here
        tex_ = 0;
        pbo_[0] = pbo_[1] = 0;
    }
    context_ = ctx;
    const Pbo& f = Funcs();
    const uint32_t token = render::PushState();
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    if (!tex_) {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
        // RGB8: the X byte of bgr0 is not alpha, so the picture is opaque whatever the helper leaves there.
        glTexImage2D(GL_TEXTURE_2D, 0, kRgb8, kTexW, kTexH, 0, kBgra, GL_UNSIGNED_BYTE, nullptr);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex_);
    }
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    bool done = false;
    if (f.ok) {
        if (!pbo_[0]) f.GenBuffers(2, pbo_);
        const GLuint b = pbo_[next_];
        next_ ^= 1;
        f.BindBuffer(kPixelUnpackBuffer, b);
        f.BufferData(kPixelUnpackBuffer, static_cast<ptrdiff_t>(bytes), nullptr, kStreamDraw);  // orphan: no stall
        if (void* dst = f.MapBuffer(kPixelUnpackBuffer, kWriteOnly)) {
            std::memcpy(dst, data, bytes);
            if (f.UnmapBuffer(kPixelUnpackBuffer)) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), kBgra, kUint8888Rev, nullptr);
                done = true;
            }
        }
        f.BindBuffer(kPixelUnpackBuffer, 0);
    }
    if (!done) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), kBgra, kUint8888Rev, data);
    glPopClientAttrib();
    render::PopState(token);
    return true;
}

void Uploader::Destroy() {
    if (context_ && context_ == wglGetCurrentContext()) {
        if (tex_) glDeleteTextures(1, &tex_);
        const Pbo& f = Funcs();
        if (pbo_[0] && f.ok) f.DeleteBuffers(2, pbo_);
    }
    tex_ = 0;
    pbo_[0] = pbo_[1] = 0;
    context_ = nullptr;
}
}  // namespace melange::media::gl
