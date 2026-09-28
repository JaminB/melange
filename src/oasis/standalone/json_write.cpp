#include "oasis/standalone/json_write.h"

#include <cstdio>

#include "tools/json_mini.h"

namespace melange::oasis::standalone {
std::string WriteJson(const json::Value& v) {
    char buf[40];
    switch (v.type) {
        case json::Type::Null: return "null";
        case json::Type::Bool: return v.boolean ? "true" : "false";
        case json::Type::Number:
            if (v.IsInteger()) {
                snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.number));
                return buf;
            }
            snprintf(buf, sizeof buf, "%.17g", v.number);
            return buf;
        case json::Type::String: return "\"" + jsonmini::Escape(v.string) + "\"";
        case json::Type::Array: {
            std::string s = "[";
            for (size_t i = 0; i < v.items.size(); ++i) s += (i ? "," : "") + WriteJson(v.items[i]);
            return s + "]";
        }
        case json::Type::Object: {
            std::string s = "{";
            bool first = true;
            for (const auto& [k, x] : v.members) {
                if (!first) s += ",";
                first = false;
                s += "\"" + jsonmini::Escape(k) + "\":" + WriteJson(x);
            }
            return s + "}";
        }
    }
    return "null";
}
}  // namespace melange::oasis::standalone
