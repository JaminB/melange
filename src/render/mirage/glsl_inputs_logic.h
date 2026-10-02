#pragma once
// Pure helpers for the extra inputs a GLSL replacement may declare: the scene copy taken before the program's first
// draw in a frame, and uniforms fed from another program's Cg parameter. Offline self-tested in
// tests/shaders_selftest.cpp.
#include <cstdint>
#include <string_view>

namespace melange::mirage::shaders::glsl::logic {
enum class SceneInput { None, Depth, Color, NearFar, View, Proj };

inline SceneInput ClassifySceneInput(std::string_view name) {
    if (name == "mg_depth") return SceneInput::Depth;
    if (name == "mg_scene") return SceneInput::Color;
    if (name == "mg_nearFar") return SceneInput::NearFar;
    if (name == "mg_view") return SceneInput::View;
    if (name == "mg_proj") return SceneInput::Proj;
    return SceneInput::None;
}

// The candidate whose values were stored last (the program the engine fed most recently); -1 when none has any.
inline int Freshest(const uint64_t* stamps, int n) {
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (stamps[i] && (best < 0 || stamps[i] > stamps[best])) best = i;
    return best;
}
}  // namespace melange::mirage::shaders::glsl::logic
