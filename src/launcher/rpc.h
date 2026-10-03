#pragma once
#include <string>

#include "melange/oasis.h"
#include "tools/json_read.h"

// Melange.exe's own Oasis methods: setup.*, launcher.*, plugins.*, defaults.*, recommended.*.
namespace melange::launcher::rpc {
void InstallSetup();
void InstallLauncher();
void InstallPlugins();

inline void Fail(oasis::Result& r, int code, const std::string& msg, const std::string& data = {}) {
    r.ok = false;
    r.code = code;
    r.message = msg;
    r.data = data;
}
inline bool Params(const oasis::Call& c, oasis::Result& r, json::Value* p) {
    json::Error e;
    if (!json::Parse(c.paramsJson, p, &e) || !p->IsObject()) {
        Fail(r, -32602, "bad params");
        return false;
    }
    return true;
}
inline std::string Str(const json::Value& p, const char* k) {
    const json::Value* v = p.Get(k);
    return v && v->IsString() ? v->string : std::string();
}
inline bool Bool(const json::Value& p, const char* k, bool def = false) {
    const json::Value* v = p.Get(k);
    return v && v->IsBool() ? v->boolean : def;
}
}  // namespace melange::launcher::rpc
