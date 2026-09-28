#pragma once
#include <string>

#include "tools/json_read.h"

// lua.eval / lua.complete parameters: {target: "client"|"match"|"mod", mod?, code|prefix}.
namespace melange::oasis::rpc {
enum class LuaTarget { Client, Match, Mod };
struct LuaRequest {
    LuaTarget target = LuaTarget::Client;
    std::string mod, text;
};
inline constexpr size_t kMaxCode = 64 * 1024, kMaxPrefix = 256, kMaxResultText = 256 * 1024;

inline bool ParseLua(const json::Value& p, const char* textKey, size_t maxText, LuaRequest* out, std::string* why) {
    const json::Value* t = p.Get("target");
    const std::string target = t && t->IsString() ? t->string : (t ? "" : "client");
    if (target == "client") out->target = LuaTarget::Client;
    else if (target == "match") out->target = LuaTarget::Match;
    else if (target == "mod") out->target = LuaTarget::Mod;
    else return *why = "target must be \"client\", \"match\" or \"mod\"", false;
    const json::Value* m = p.Get("mod");
    if (out->target == LuaTarget::Mod) {
        if (!m || !m->IsString() || m->string.empty()) return *why = "mod is required with target \"mod\"", false;
        out->mod = m->string;
    } else if (m && !m->IsNull()) {
        return *why = "mod is only used with target \"mod\"", false;
    }
    const json::Value* x = p.Get(textKey);
    if (!x || !x->IsString()) return *why = std::string(textKey) + " must be a string", false;
    if (x->string.size() > maxText) return *why = std::string(textKey) + " is longer than " + std::to_string(maxText / 1024 ? maxText / 1024 : maxText) + (maxText >= 1024 ? " KB" : " bytes"), false;
    if (x->string.find('\0') != std::string::npos) return *why = std::string(textKey) + " contains a NUL character", false;
    out->text = x->string;
    return true;
}

inline std::string ClipText(std::string s, size_t max = kMaxResultText) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;
    s.resize(n);
    s += "\n... (output truncated)";
    return s;
}
}  // namespace melange::oasis::rpc
