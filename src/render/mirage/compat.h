#pragma once
// GPU compatibility report: collection (main thread) and the text/JSON forms for the overlay and the log export.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "melange/compat.h"

namespace melange::mirage::compat {
using namespace melange::compat;

struct ProgramRow {
    std::string file, entry, owner, reason;
    int stage = 0;
    bool failed = false, overridden = false, glsl = false, pending = false;
    uint32_t binds = 0;
};
struct EffectRow {
    std::string id, title, stage;
    bool enabled = false, failed = false;
};
struct StageRow {
    std::string name;
    int bucket = 0;
    bool post = false, installed = false;
    uint64_t calls = 0;
};
struct Snapshot {
    Gpu gpu{};
    std::string driverProvider, driverDate, adapter, contextProfile, cgVersion, cgLatestVertex, cgLatestFragment, traceMode;
    int contextFlags = 0, maxTextureSize = 0, maxTextureUnits = 0, maxTextureImageUnits = 0;
    std::vector<std::string> extensions, wglExtensions;
    std::vector<std::pair<std::string, bool>> cgProfiles;  // name, cgGLIsProfileSupported
    bool engineKnown = false, fxaa = false, msaa = false, debugContext = false, hub = false;
    bool programsLoaded = false;  // the engine's shader manager exists
    std::vector<ProgramRow> programs;
    uint32_t passthroughLoads = 0, overriddenPrograms = 0, compileErrors = 0;
    std::vector<EffectRow> effects;
    bool postfxBypassed = false;
    std::string bypassReason;
    std::vector<StageRow> stages;
    uint64_t frame = 0;
};

// compat_report.cpp (no GL; self-tested)
void SetSnapshot(Snapshot s);
Snapshot GetSnapshot();
std::vector<Entry> Entries();
std::string BuildJson(const Snapshot& s, const std::vector<Entry>& reports);
std::string BuildText(const Snapshot& s, const std::vector<Entry>& reports);
const char* KindName(Kind k);
const char* StatusName(Status s);
std::string Json();  // any thread; the last snapshot plus every report
std::string Text();

// compat.cpp
void Install();  // MirageTrace::Install: frame collection, panel "Mirage/GPU", verb compat.report
}  // namespace melange::mirage::compat
