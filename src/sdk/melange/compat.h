#pragma once
#include <cstddef>
#include <cstdint>
namespace melange::compat {
// The GPU compatibility report: what loaded, was skipped or failed on this machine, and why. Shown in the overlay
// panel "Mirage/GPU" and written into the Save-logs bundle (gpu/compat.json, gpu/compat.txt).
enum class Kind : uint8_t { Shader, Pass, Effect, Feature };
enum class Status : uint8_t { Loaded, Skipped, Failed };

// Any thread. A later report for the same (kind, id) replaces the earlier one.
// id: e.g. "PostProcess.cg:CopyFxaa", "mirage-samples/bloom", "gldebug"; reason: short text, may be null;
// owner: "builtin", a mod id, or null.
void Report(Kind kind, const char* id, Status status, const char* reason = nullptr, const char* owner = nullptr);
void Forget(Kind kind, const char* id);

struct Entry {
    Kind kind;
    Status status;
    char id[96], owner[48], reason[192];
    uint64_t frame;  // frame of the last report
};
size_t List(Entry* out, size_t max);  // any thread (copy), in first-report order

struct Gpu {
    char vendor[64], renderer[128], version[128], glsl[64];
    char driver[64];                 // display driver version from the registry ("" under Wine)
    char cgVertex[16], cgFragment[16];  // the profiles the engine compiles for ("arbvp1" / "arbfp1")
    int extensions;
    bool valid;                      // false until the first frame with a GL context
};
Gpu GetGpu();  // any thread (copy)
}
