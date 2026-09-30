#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "erg/scene.h"
#include "xom/json.h"

// Shared JSON helpers of the scene and patch codecs (internal).
namespace melange::erg::jsonio {
using xom::Json;

std::string Compact(const Json& v);                   // no whitespace; numbers as their literal text
Json Num(double v);                                   // shortest round-trip text; integers without ".0"
Json Int(int64_t v);
Json Vec(const Vec3& v);
Json Str(std::string_view s);

// Readers set *err ("<path>: <why>") and return false; the path is passed in by the caller.
bool Obj(const Json* v, const std::string& path, std::string* err);
bool GetString(const Json& o, const char* key, const std::string& path, std::string* out, std::string* err,
               bool required = true);
bool GetInt(const Json& o, const char* key, const std::string& path, int64_t lo, int64_t hi, int64_t* out,
            std::string* err, bool required = true);
bool GetNumber(const Json& v, const std::string& path, double lo, double hi, double* out, std::string* err);
bool GetVec(const Json& o, const char* key, const std::string& path, Vec3* out, std::string* err, bool required,
            double limit = 1e6);
bool OnlyKeys(const Json& o, std::initializer_list<std::string_view> keys, const std::string& path, std::string* err);
bool IsHex64(std::string_view s);

// erg-scene/2 and erg-patch/2 share these blocks.
bool GetBool(const Json& o, const char* key, const std::string& path, bool* out, std::string* err, bool required = true);
bool ParseObjects(const Json* arr, std::vector<ObjectSpec>* out, std::string* err);
Json ObjectsJson(const std::vector<ObjectSpec>& objects);
bool ParseKind(const Json* o, bool* survivor, std::string* err);
Json KindJson(bool survivor);
bool ParseScript(const Json* o, ScriptMeta* out, std::string* err);
Json ScriptJson(const ScriptMeta& m);
}  // namespace melange::erg::jsonio
