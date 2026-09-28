#pragma once
#include <string>
#include <string_view>

#include "melange/oasis.h"
#include "tools/json_read.h"

// Parameter helpers shared by the RPC handlers.
namespace melange::oasis::rpc {
inline constexpr int kBadParams = -32602, kRefused = -32000, kNotInMatch = -32001, kBusy = -32002;

inline bool Fail(Result& r, int code, std::string msg) {
    r.ok = false;
    r.code = code;
    r.message = std::move(msg);
    return false;
}

inline bool ParseParams(const Call& c, json::Value* v, Result& r) {
    json::Error e;
    if (json::Parse(c.paramsJson.empty() ? std::string_view("{}") : c.paramsJson, v, &e) && v->IsObject()) return true;
    return Fail(r, kBadParams, "params must be a JSON object");
}

// A string member; `required` members that are missing or of another type fail with -32602.
inline bool Str(const json::Value& v, const char* key, std::string* out, Result& r, bool required = true) {
    const json::Value* m = v.Get(key);
    if (!m || m->IsNull()) {
        if (!required) return true;
        return Fail(r, kBadParams, std::string(key) + " is required");
    }
    if (!m->IsString()) return Fail(r, kBadParams, std::string(key) + " must be a string");
    *out = m->string;
    return true;
}

inline bool Flag(const json::Value& v, const char* key, bool* out, Result& r) {
    const json::Value* m = v.Get(key);
    if (!m || !m->IsBool()) return Fail(r, kBadParams, std::string(key) + " must be true or false");
    *out = m->boolean;
    return true;
}
}  // namespace melange::oasis::rpc
