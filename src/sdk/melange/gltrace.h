#pragma once
#include <cstdint>
#include <string>
namespace melange::gltrace {
enum class Mode : uint8_t { Off, Count, Log };  // Off = passthrough thunks (or none when nothing needs the hub)
bool Installed();
Mode GetMode();
void SetMode(Mode m);  // any thread; takes effect at the next frame boundary

enum Source : uint8_t { kExeImport = 1, kExeProc = 2, kCgGLImport = 4, kCgGLProc = 8 };
struct FnStat { const char* name; uint8_t source; double perFrame; uint32_t lastFrame; };
struct FrameStats {
    uint64_t frame;
    uint32_t calls, draws, cgCalls, programSwitches, paramFlushes, fboBinds, texBinds, texUploads, getErrors;
    double busyMs;
};
FrameStats Last();                                      // the last completed frame (Count or Log mode)
FrameStats Average(uint32_t frames);                    // mean over the last N (<= 600)
size_t Top(FnStat* out, size_t max);                    // by perFrame, descending
uint32_t ProcsHandedOut();                              // wglGetProcAddress results wrapped

struct CaptureOptions {
    uint32_t frames = 1;          // consecutive frames (1..8)
    bool textures = true;         // every texture bound during the frame, level 0, PNG
    bool shaders = true;          // Cg source path, entry, compiled ARB/GLSL text, Melange replacements
    bool frameImage = true;       // back buffer before the overlay
    bool bufferSizes = true;      // VBO/IBO sizes only, never contents
    uint32_t maxTextureMB = 256;
};
// Arms a capture that starts at the next Frame event. Writes Documents\Melange\captures\<stamp>.mcap.
bool RequestCapture(const CaptureOptions& opt);  // any thread; false if one is running
enum class CaptureState : uint8_t { Idle, Armed, Recording, Writing, Done, Failed };
CaptureState CaptureStatus(std::wstring* path = nullptr, std::string* error = nullptr);

// Texture dumper: level 0 of each engine texture upload as PNG, deduplicated by content hash.
bool StartTextureDump(uint32_t maxCount, const char* nameFilter = nullptr);  // substring on the image name; any thread
void StopTextureDump();
uint32_t TexturesDumped();

// GPU timer queries (L0): swap-to-swap and each Mirage stage (render::Stage World..Final, offset by one).
// [Mirage] GpuTimers=1 (default) controls whether these run at all; off by itself costs nothing.
enum class GpuRegion : uint8_t { Swap = 0, World, WorldLate, PostWorld, Hud, Final, Count };
struct GpuTime { double ms; bool valid; };  // valid is false until the GPU has returned a first result for it
bool GpuTimerSupported();                   // a GL context with GL_ARB_timer_query (or GL 3.3+) is current
GpuTime GetGpuTime(GpuRegion r);
}
