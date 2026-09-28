#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/events.h"
#include "render/mirage/compat.h"
#include "tools/json_mini.h"

namespace melange::mirage::compat {
namespace {
std::mutex g_mx;
std::vector<Entry> g_entries;
Snapshot g_snap;

void Copy(char* dst, size_t n, const char* src) {
    if (!src) src = "";
    strncpy_s(dst, n, src, _TRUNCATE);
}

std::string Line(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return std::string(b) + "\n";
}

const Entry* FindReport(const std::vector<Entry>& r, Kind k, const std::string& id) {
    for (const Entry& e : r)
        if (e.kind == k && id == e.id) return &e;
    return nullptr;
}
}  // namespace

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Shader: return "shader";
        case Kind::Pass: return "pass";
        case Kind::Effect: return "effect";
        default: return "feature";
    }
}

const char* StatusName(Status s) {
    switch (s) {
        case Status::Loaded: return "loaded";
        case Status::Skipped: return "skipped";
        default: return "failed";
    }
}

void SetSnapshot(Snapshot s) {
    std::lock_guard lk(g_mx);
    g_snap = std::move(s);
}

Snapshot GetSnapshot() {
    std::lock_guard lk(g_mx);
    return g_snap;
}

std::vector<Entry> Entries() {
    std::lock_guard lk(g_mx);
    return g_entries;
}

std::string BuildJson(const Snapshot& s, const std::vector<Entry>& reports) {
    jsonmini::Obj gpu;
    gpu.Bool("valid", s.gpu.valid).Str("vendor", s.gpu.vendor).Str("renderer", s.gpu.renderer).Str("version", s.gpu.version)
        .Str("glsl", s.gpu.glsl).Str("adapter", s.adapter).Str("driverVersion", s.gpu.driver).Str("driverDate", s.driverDate)
        .Str("driverProvider", s.driverProvider).Int("contextFlags", s.contextFlags).Str("contextProfile", s.contextProfile)
        .Int("maxTextureSize", s.maxTextureSize).Int("maxTextureUnits", s.maxTextureUnits)
        .Int("maxTextureImageUnits", s.maxTextureImageUnits);
    jsonmini::Arr ext, wext;
    for (const auto& e : s.extensions) ext.Str(e);
    for (const auto& e : s.wglExtensions) wext.Str(e);
    gpu.Int("extensionCount", static_cast<long long>(s.extensions.size())).Raw("extensions", ext.End()).Raw("wglExtensions", wext.End());

    jsonmini::Arr profiles;
    for (const auto& [name, ok] : s.cgProfiles) {
        jsonmini::Obj o;
        o.Str("profile", name).Bool("supported", ok);
        profiles.Raw(o.End());
    }
    jsonmini::Obj cg;
    cg.Str("dllVersion", s.cgVersion).Str("engineVertex", s.gpu.cgVertex).Str("engineFragment", s.gpu.cgFragment)
        .Str("latestVertex", s.cgLatestVertex).Str("latestFragment", s.cgLatestFragment).Raw("profiles", profiles.End());

    jsonmini::Obj engine;
    engine.Bool("knownBuild", s.engineKnown).Bool("fxaa", s.fxaa).Bool("msaa", s.msaa).Bool("debugContext", s.debugContext)
        .Bool("glHub", s.hub).Str("traceMode", s.traceMode);

    jsonmini::Arr progs;
    uint32_t failed = 0;
    for (const ProgramRow& p : s.programs) {
        std::string id = p.file + ":" + p.entry;
        const Entry* r = FindReport(reports, Kind::Shader, id);
        const char* status = p.failed ? "failed" : r ? StatusName(r->status) : "loaded";
        std::string reason = !p.reason.empty() ? p.reason : r ? r->reason : "";
        failed += p.failed;
        jsonmini::Obj o;
        o.Str("file", p.file).Str("entry", p.entry).Str("stage", p.stage == 0 ? "vertex" : "fragment").Str("status", status)
            .Str("reason", reason).Str("owner", !p.owner.empty() ? p.owner : r ? r->owner : "").Bool("overridden", p.overridden)
            .Bool("glsl", p.glsl).Bool("pendingReload", p.pending).UInt("binds", p.binds);
        progs.Raw(o.End());
    }
    jsonmini::Obj shaders;
    shaders.Bool("managerPresent", s.programsLoaded).Int("programs", static_cast<long long>(s.programs.size()))
        .UInt("failed", failed).UInt("passthroughLoads", s.passthroughLoads).UInt("overridden", s.overriddenPrograms)
        .UInt("compileErrors", s.compileErrors).Raw("list", progs.End());

    jsonmini::Arr effects;
    for (const EffectRow& e : s.effects) {
        jsonmini::Obj o;
        const Entry* r = FindReport(reports, Kind::Effect, e.id);
        const char* status = e.failed ? "failed" : !e.enabled ? "skipped" : "loaded";
        std::string reason = r ? r->reason : e.failed ? "" : !e.enabled ? "disabled" : "";
        o.Str("id", e.id).Str("title", e.title).Str("stage", e.stage).Str("status", status).Str("reason", reason);
        effects.Raw(o.End());
    }
    jsonmini::Arr stages;
    for (const StageRow& st : s.stages) {
        jsonmini::Obj o;
        o.Str("stage", st.name).Int("bucket", st.bucket).Bool("post", st.post).Bool("installed", st.installed).UInt("calls", st.calls);
        stages.Raw(o.End());
    }
    jsonmini::Obj passes;
    passes.Bool("postfxBypassed", s.postfxBypassed).Str("bypassReason", s.bypassReason).Raw("effects", effects.End())
        .Raw("stages", stages.End());

    jsonmini::Arr reps;
    for (const Entry& e : reports) {
        jsonmini::Obj o;
        o.Str("kind", KindName(e.kind)).Str("id", e.id).Str("status", StatusName(e.status)).Str("reason", e.reason)
            .Str("owner", e.owner).UInt("frame", e.frame);
        reps.Raw(o.End());
    }
    jsonmini::Obj root;
    root.Str("format", "melange-gpu-compat").Int("version", 1).UInt("frame", s.frame).Raw("gpu", gpu.End()).Raw("cg", cg.End())
        .Raw("engine", engine.End()).Raw("shaders", shaders.End()).Raw("passes", passes.End()).Raw("reports", reps.End());
    return root.End();
}

std::string BuildText(const Snapshot& s, const std::vector<Entry>& reports) {
    std::string t = "Melange GPU compatibility report\n\n";
    if (!s.gpu.valid) t += "No GL context seen yet.\n";
    t += Line("GPU:      %s (%s)", s.gpu.renderer, s.gpu.vendor);
    t += Line("Adapter:  %s, driver %s (%s) %s", s.adapter.c_str(), s.gpu.driver, s.driverDate.c_str(), s.driverProvider.c_str());
    t += Line("OpenGL:   %s, GLSL %s, %s, flags 0x%x", s.gpu.version, s.gpu.glsl, s.contextProfile.c_str(), s.contextFlags);
    t += Line("Limits:   texture %d, units %d, image units %d; %zu GL and %zu WGL extensions", s.maxTextureSize,
              s.maxTextureUnits, s.maxTextureImageUnits, s.extensions.size(), s.wglExtensions.size());
    t += Line("Cg:       cg.dll %s; engine compiles for %s / %s; latest %s / %s", s.cgVersion.c_str(), s.gpu.cgVertex,
              s.gpu.cgFragment, s.cgLatestVertex.c_str(), s.cgLatestFragment.c_str());
    std::string sup, unsup;
    for (const auto& [name, ok] : s.cgProfiles) (ok ? sup : unsup) += (ok ? sup : unsup).empty() ? name : " " + name;
    t += Line("Cg profiles supported: %s", sup.empty() ? "-" : sup.c_str());
    t += Line("Cg profiles not supported: %s", unsup.empty() ? "-" : unsup.c_str());
    t += Line("Engine:   known build %s, FXAA %s, MSAA %s, debug context %s, GL hub %s (%s)", s.engineKnown ? "yes" : "no",
              s.fxaa ? "on" : "off", s.msaa ? "on" : "off", s.debugContext ? "yes" : "no", s.hub ? "yes" : "no",
              s.traceMode.c_str());

    uint32_t failed = 0;
    for (const ProgramRow& p : s.programs) failed += p.failed;
    t += Line("\nShaders: %zu programs, %u failed, %u overridden, %u passthrough loads, %u compile errors%s", s.programs.size(),
              failed, s.overriddenPrograms, s.passthroughLoads, s.compileErrors,
              s.programsLoaded ? "" : " (the engine has not created its shader manager yet)");
    for (const ProgramRow& p : s.programs) {
        const Entry* r = FindReport(reports, Kind::Shader, p.file + ":" + p.entry);
        const char* status = p.failed ? "FAILED" : r ? StatusName(r->status) : "loaded";
        std::string reason = !p.reason.empty() ? p.reason : r ? r->reason : "";
        std::string owner = !p.owner.empty() ? p.owner : r ? r->owner : "";
        t += Line("  %-8s %-2s %s:%s%s%s%s%s", status, p.stage == 0 ? "vp" : "fp", p.file.c_str(), p.entry.c_str(),
                  owner.empty() ? "" : " [", owner.c_str(), owner.empty() ? "" : "]", reason.empty() ? "" : (" - " + reason).c_str());
    }
    t += Line("\nPost-FX: %zu effects%s%s", s.effects.size(), s.postfxBypassed ? ", BYPASSED: " : "",
              s.postfxBypassed ? s.bypassReason.c_str() : "");
    for (const EffectRow& e : s.effects) {
        const Entry* r = FindReport(reports, Kind::Effect, e.id);
        std::string reason = r ? r->reason : e.failed ? "" : !e.enabled ? "disabled" : "";
        t += Line("  %-8s %s (%s, %s)%s", e.failed ? "FAILED" : e.enabled ? "loaded" : "skipped", e.id.c_str(), e.title.c_str(),
                  e.stage.c_str(), reason.empty() ? "" : (" - " + reason).c_str());
    }
    t += "\nStages:\n";
    for (const StageRow& st : s.stages)
        t += Line("  %-9s %s[%d] installed=%d calls=%llu", st.name.c_str(), st.post ? "post" : "pre", st.bucket, st.installed,
                  static_cast<unsigned long long>(st.calls));
    t += "\nReports:\n";
    for (const Entry& e : reports)
        t += Line("  %-8s %-7s %s%s%s%s%s", StatusName(e.status), KindName(e.kind), e.id, *e.owner ? " [" : "", e.owner,
                  *e.owner ? "]" : "", *e.reason ? (std::string(" - ") + e.reason).c_str() : "");
    t += "\nGL extensions:\n";
    for (const auto& e : s.extensions) t += "  " + e + "\n";
    t += "\nWGL extensions:\n";
    for (const auto& e : s.wglExtensions) t += "  " + e + "\n";
    return t;
}

std::string Json() { return BuildJson(GetSnapshot(), Entries()); }
std::string Text() { return BuildText(GetSnapshot(), Entries()); }
}  // namespace melange::mirage::compat

namespace melange::compat {
namespace c = mirage::compat;

void Report(Kind kind, const char* id, Status status, const char* reason, const char* owner) {
    if (!id || !*id) return;
    std::lock_guard lk(c::g_mx);
    Entry* e = nullptr;
    for (Entry& x : c::g_entries)
        if (x.kind == kind && strncmp(x.id, id, sizeof x.id - 1) == 0) e = &x;
    if (!e) {
        if (c::g_entries.size() >= 4096) return;
        e = &c::g_entries.emplace_back();
        e->kind = kind;
        c::Copy(e->id, sizeof e->id, id);
    }
    e->status = status;
    c::Copy(e->reason, sizeof e->reason, reason);
    c::Copy(e->owner, sizeof e->owner, owner);
    e->frame = events::FrameCount();
}

void Forget(Kind kind, const char* id) {
    if (!id) return;
    std::lock_guard lk(c::g_mx);
    std::erase_if(c::g_entries, [&](const Entry& x) { return x.kind == kind && strncmp(x.id, id, sizeof x.id - 1) == 0; });
}

size_t List(Entry* out, size_t max) {
    std::lock_guard lk(c::g_mx);
    size_t n = out ? std::min(max, c::g_entries.size()) : 0;
    std::copy_n(c::g_entries.begin(), n, out);
    return n;
}

Gpu GetGpu() {
    std::lock_guard lk(c::g_mx);
    return c::g_snap.gpu;
}
}  // namespace melange::compat
