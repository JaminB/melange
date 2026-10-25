#pragma once
// The media host (melange.asi side of wum.media): players, the helper process, the pipe, the frame section, the GL
// upload and p:follow. Written as a contract by Melange 0.12.0 C0; C4b implements it in media.cpp and the files it owns
// (helper.cpp, pipe.cpp, shm.cpp, upload_gl.cpp, media_sync.h, ytdlp lane hooks). The functions below are the ones
// OTHER subsystems call; everything wum_media.cpp needs beyond them is C4b's own business.
#include <cstdint>

namespace melange::media {
// Present hook, first thing in overlay's OnPresent (before the screen stage and ImGui), every frame whether the
// overlay is shown or not. Uploads new frames of players drawn in the last 250 ms. Main thread, GL context current;
// must leave GL state as it found it (gl_guard / render::PushState). Never blocks on the helper.
void OnPresent();

// For wum.draw.media (C5). The texture that holds `playerId`'s latest frame, if the player belongs to `modId` and is
// open. Marks the player as drawn this frame (so OnPresent keeps uploading). False: not this mod's player, closed, or
// no frame uploaded yet (C5 then draws nothing and wum.draw.media returns nil).
struct DrawFrame {
    unsigned texture = 0;   // GL_TEXTURE_2D, allocated at 1280x720, linear, clamp to edge
    int w = 0, h = 0;       // the frame's pixels inside the texture (from the origin)
    float u1 = 0, v1 = 0;   // w / 1280, h / 720: the UV of the frame's far corner
    double aspect = 0;      // display aspect (video-params), or w / h
};
bool FrameForDraw(const char* modId, uint32_t playerId, DrawFrame* out);
}  // namespace melange::media
