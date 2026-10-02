#pragma once
namespace melange::graphics {
// Shadow-map size (MirageShadows). A mod's runtime request replaces its spice.json graphics.shadowMapSize while it
// is set: size 512/1024/2048/4096, 0 = no request, -1 = back to the manifest's. Changes rebuild the engine's
// shadow map on the next frame. False for an unknown size or while the feature is unavailable.
bool SetShadowMapRequest(const char* mod, int size);
struct ShadowMapInfo {
    int size;        // the engine's current shadow-map size
    int effective;   // merged request after the ini override; 0 = vanilla
    int vanilla;     // the size from the engine's own cfg files (0 until known)
    bool modRequest; // an enabled mod asks for a size
    bool available;  // the engine's shadow-map code was recognised
};
ShadowMapInfo GetShadowMapInfo();
}
