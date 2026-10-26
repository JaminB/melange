#pragma once
// The size the software renderer draws at: the video's display aspect fitted into the core's size hint, never larger
// than the video itself (GL scales up for free, the CPU does not) and never larger than a slot (1280x720). The width
// is a multiple of 16, so the stride (w*4, which the core requires exactly) is 64-byte aligned for mpv's SIMD paths.
#include "media/media_proto.h"

namespace holo {
struct Dims {
    int w = 0, h = 0;
};

// box: the core's size hint (0 = none yet). video: decoded display size (0 = unknown). aspect: display aspect, 0 = from
// the video size or 16:9.
inline Dims FitFrame(int boxW, int boxH, int videoW, int videoH, double aspect) {
    constexpr int kMaxW = static_cast<int>(melange::media::proto::kFrameMaxW);
    constexpr int kMaxH = static_cast<int>(melange::media::proto::kFrameMaxH);
    if (boxW <= 0 || boxH <= 0) {
        boxW = 960;  // the half-screen panel at 1080p, until the core says otherwise
        boxH = 540;
    }
    if (boxW > kMaxW) boxW = kMaxW;
    if (boxH > kMaxH) boxH = kMaxH;
    if (videoW > 0 && videoH > 0) {
        if (!(aspect > 0.05 && aspect < 20)) aspect = static_cast<double>(videoW) / videoH;
        // the video's own display size bounds the box (no CPU upscaling)
        const int dispW = static_cast<int>(videoH * aspect + 0.5);
        if (dispW > 0 && dispW < boxW) boxW = dispW;
        if (videoH < boxH) boxH = videoH;
    }
    if (!(aspect > 0.05 && aspect < 20)) aspect = 16.0 / 9.0;
    int w = boxW, h = static_cast<int>(boxW / aspect + 0.5);
    if (h > boxH) {
        h = boxH;
        w = static_cast<int>(boxH * aspect + 0.5);
    }
    w &= ~15;
    if (w < 16) w = 16;
    const int hw = static_cast<int>(w / aspect + 0.5);  // the height of the rounded width keeps the aspect
    if (hw < h) h = hw;
    h &= ~1;
    if (h < 2) h = 2;
    if (w > kMaxW) w = kMaxW;
    if (h > kMaxH) h = kMaxH;
    return {w, h};
}
}  // namespace holo
