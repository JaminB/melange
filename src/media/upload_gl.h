#pragma once
// A player's video texture (design 1.1.5): allocated once at 1280x720 (RGB8, linear, no mips, clamp to edge) and fed
// a frame at a time through two orphaned pixel buffer objects, or a direct glTexSubImage2D when the context has none.
// Main thread, game GL context current; every call leaves GL state as it found it (render::PushState).
#include <cstdint>

namespace melange::media::gl {
class Uploader {
public:
    // Copies a checked BGR0 frame (stride w*4, w <= 1280, h <= 720) into the texture's top-left corner.
    bool Upload(const uint8_t* data, uint32_t w, uint32_t h);
    unsigned Texture() const { return tex_; }
    void Destroy();  // frees the texture and the buffers (main thread, same context)

private:
    unsigned tex_ = 0;
    unsigned pbo_[2] = {0, 0};
    int next_ = 0;
    void* context_ = nullptr;  // HGLRC the objects belong to; another context means start over
};
}  // namespace melange::media::gl
