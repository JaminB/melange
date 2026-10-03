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

// Supersampling (MirageSupersample): the scene renders at a multiple of the window size and Composite scales it
// down. A mod's runtime request: 2 (1x2) or 4 (2x2) samples per pixel, 0 = no request. Changes rebuild the scene
// targets on the next frame. False for another count or while the feature is unavailable.
bool SetSupersampleRequest(const char* mod, int samples);
struct SupersampleInfo {
    int x, y;          // the engine's current factors
    int effective;     // merged request after the ini override, in samples; 0 = vanilla
    int sceneW, sceneH;
    bool multisampled; // /SSAA is using multisampled renderbuffers (hardware AA) rather than larger targets
    bool modRequest;   // an enabled mod asks for supersampling
    bool available;    // the engine's /SSAA switch was recognised
};
SupersampleInfo GetSupersampleInfo();
}
