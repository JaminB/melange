#pragma once
#include <cstdint>
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

// Added by B (additive): per-mod state for the Mods page. False if the Sandbox never saw the mod.
struct ModStatus {
    bool loaded = false;
    std::string error;               // last load or reload error ("" if none), with the chunk line number
    uint32_t callbacks = 0, disabledCallbacks = 0, faults = 0;
    uint64_t bytes = 0, instructions = 0;
    double msLastFrame = 0;
};
bool Status(const char* id, ModStatus* out);
}
