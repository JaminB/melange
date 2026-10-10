#pragma once
// Distance falloff for positional sounds. Linear between Near and Far (world units), full volume inside Near,
// silent beyond Far. Pure so the self-test covers it.
#include <algorithm>
#include <cmath>

namespace melange::audio {
// 1 inside `nearD`, 0 at and beyond `farD`, linear between. If farD <= nearD there is no ramp: a hard cut at nearD.
// A NaN distance (a garbage camera or position) is silent rather than full volume.
inline float Attenuation(float dist, float nearD, float farD) {
    if (std::isnan(dist)) return 0.0f;
    if (dist < 0.0f) dist = 0.0f;
    if (!(farD > nearD)) return dist <= nearD ? 1.0f : 0.0f;
    return std::clamp(1.0f - (dist - nearD) / (farD - nearD), 0.0f, 1.0f);
}

inline float Distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline float PositionalVolume(float volume, float dist, float nearD, float farD) {
    return volume * Attenuation(dist, nearD, farD);
}
}  // namespace melange::audio
