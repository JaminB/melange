#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "launcher/plugin_settings.h"
#include "store/index.h"

// The plugins first run suggests: the store index's optional "recommended" list, or a built-in one offline.
namespace melange::launcher {
struct Recommended {
    std::string id, name, description, why;
    plugins::Values settings;
    std::vector<plugins::Setting> decl;
    bool installed = false, compatible = true;
    std::string reason;
};
std::vector<Recommended> BuiltinRecommended();
// The declaration known for a plugin without its spice.json at hand (the built-in ones), or false.
bool BuiltinDecl(const std::string& id, std::vector<plugins::Setting>* out);
using DeclLookup = std::function<bool(const std::string& id, std::vector<plugins::Setting>* out)>;
// False when the index text has no "recommended" key. Entries whose id is not in `idx` or whose settings do not
// match the declaration are dropped.
bool ParseRecommended(std::string_view indexText, const store::Index& idx, std::vector<Recommended>* out,
                      const DeclLookup& decl = BuiltinDecl);
std::string RecommendedJson(const Recommended& r);
}  // namespace melange::launcher
