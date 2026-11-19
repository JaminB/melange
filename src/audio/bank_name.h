#pragma once
// Speech bank names (Melange 0.17): what wum.audio.voice accepts and what wum.game.teams() reports as a team's voice.
// Header-only and pure, so the game-state reader and the self-tests use it without the decoder.
#include <cstddef>
#include <string>

namespace melange::audio::fsb {
// A bank name a mod may ask for: 1-32 of [A-Za-z0-9_-], starting with "vo" in any case (the speech banks).
inline bool ValidBankName(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return name.size() >= 2 && (name[0] == 'v' || name[0] == 'V') && (name[1] == 'o' || name[1] == 'O');
}

// The team's SfxBankName as the engine stores it, reduced to a bank name: the part after the last '/' or '\', without
// a file extension. Empty when what is left is not a ValidBankName.
inline std::string NormalizeBankName(const std::string& raw) {
    const size_t slash = raw.find_last_of("/\\");
    std::string s = slash == std::string::npos ? raw : raw.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s.resize(dot);
    return ValidBankName(s) ? s : std::string();
}
}  // namespace melange::audio::fsb
