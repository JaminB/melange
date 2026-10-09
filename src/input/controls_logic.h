#pragma once
// Pure logic of the Controls module (invert modes, sensitivity carry, key labels), free of game and Windows
// dependencies so tests/controls_selftest.cpp can run it.
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace melange::controls {
// The invert flag (mapping byte +0xd) that means "standard": mouse up looks/aims up. One constant per message.
// These are the initial values; they are confirmed in game in Phase 4 and flipped here if wrong.
constexpr uint8_t kStandardCameraInvertY = 1;  // Camera.MouseMoved
constexpr uint8_t kStandardAimInvertY = 0;     // Input.AimMouse

enum class InvertMode : uint8_t { Game, Standard, Inverted };

inline bool ParseInvertMode(const char* s, InvertMode* out) {
    if (!s || !out) return false;
    if (strcmp(s, "game") == 0) *out = InvertMode::Game;
    else if (strcmp(s, "standard") == 0) *out = InvertMode::Standard;
    else if (strcmp(s, "inverted") == 0) *out = InvertMode::Inverted;
    else return false;
    return true;
}

inline const char* InvertModeName(InvertMode m) {
    return m == InvertMode::Standard ? "standard" : m == InvertMode::Inverted ? "inverted" : "game";
}

// The flag byte to write for `m`, or -1 to leave the game's own flag. `standardFlag` is 0 or 1.
inline int InvertFlagFor(InvertMode m, uint8_t standardFlag) {
    switch (m) {
        case InvertMode::Standard: return standardFlag & 1;
        case InvertMode::Inverted: return (standardFlag & 1) ^ 1;
        default: return -1;
    }
}

constexpr float kMinSensitivity = 0.25f, kMaxSensitivity = 3.0f;
inline float ClampSensitivity(double v) {
    if (!(v == v)) return 1.0f;  // NaN
    if (v < kMinSensitivity) return kMinSensitivity;
    if (v > kMaxSensitivity) return kMaxSensitivity;
    return static_cast<float>(v);
}

struct Options {
    InvertMode cameraInvertY = InvertMode::Game;
    InvertMode aimInvertY = InvertMode::Game;
    bool blimpInvert = false;
    float cameraSensitivity = 1.0f;
    float aimSensitivity = 1.0f;
};

// Scales an integer motion by a float, keeping the fraction that truncation toward zero drops so slow motion is not
// lost. The residue stays in (-1, 1); at scale 1 an int passes through unchanged.
struct Carry {
    float residue = 0.0f;
    int Apply(int v, float scale) {
        const float f = static_cast<float>(v) * scale + residue;
        const int out = static_cast<int>(f);  // toward zero
        residue = f - static_cast<float>(out);
        return out;
    }
    void Reset() { residue = 0.0f; }
};

// "SPACE" -> "Space", "LCONTROL" -> "Ctrl": a short label from a keys.h name.
inline std::string KeyLabel(const char* name) {
    if (!name || !*name) return {};
    static const struct { const char* from; const char* to; } kSpecial[] = {
        {"LCONTROL", "Ctrl"}, {"RCONTROL", "RCtrl"}, {"LSHIFT", "Shift"}, {"RSHIFT", "RShift"}, {"LMENU", "Alt"},
        {"RMENU", "RAlt"},    {"RETURN", "Enter"},   {"ESCAPE", "Esc"},   {"BACK", "Backspace"}, {"CAPITAL", "Caps"}};
    for (const auto& s : kSpecial)
        if (_stricmp(name, s.from) == 0) return s.to;
    std::string out(name);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(i == 0 ? toupper(static_cast<unsigned char>(out[i])) : tolower(static_cast<unsigned char>(out[i])));
    return out;
}
}  // namespace melange::controls
