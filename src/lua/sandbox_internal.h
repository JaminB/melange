#pragma once
#include <string>
#include <vector>

// What Thumper and the console call on the Sandbox (client VM).
namespace melange::sandbox {
bool LoadMod(const char* id);        // builds the environment, runs entry.client; false keeps the previous version
void UnloadMod(const char* id);      // revokes every handle the mod holds
bool ReloadMod(const char* id);
struct EvalOut { bool ok; std::string text; };
EvalOut Eval(const char* modIdOrNull, const std::string& code);  // console; nullptr = the console environment
void Complete(const char* modIdOrNull, const std::string& prefix, std::vector<std::string>* out);
}
