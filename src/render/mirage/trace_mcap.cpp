// The .mcap v1 writer (docs/capture-format.md). No GL or game access: runs on the worker thread and offline.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "miniz.h"
#include "render/mirage/trace_internal.h"
#include "tools/json_mini.h"

namespace melange::mirage::trace {
namespace {
struct Zip {
    mz_zip_archive z{};
    FILE* f = nullptr;
    std::vector<std::string> files;
    bool ok = true;
    bool Open(const std::wstring& path) {
        f = _wfopen(path.c_str(), L"wb");
        return f && mz_zip_writer_init_cfile(&z, f, 0);
    }
    void Add(const std::string& name, const std::string& data, bool compress = true) {
        if (!ok) return;
        ok = mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), compress ? MZ_BEST_SPEED : MZ_NO_COMPRESSION) != 0;
        files.push_back(name);
    }
    bool Close() {
        bool fin = ok && mz_zip_writer_finalize_archive(&z);
        mz_zip_writer_end(&z);
        if (f) fclose(f);
        f = nullptr;
        return fin;
    }
};

const char* StageName(int s) { return s == 0 ? "vertex" : "fragment"; }

std::string FlipPng(McapTexture& t) {
    std::string png;
    if (t.depth) {
        std::vector<uint16_t> flip(t.px16.size());
        for (int y = 0; y < t.h; ++y)
            memcpy(&flip[static_cast<size_t>(y) * t.w], &t.px16[static_cast<size_t>(t.h - 1 - y) * t.w], static_cast<size_t>(t.w) * 2);
        png = PngGrey16(flip.data(), t.w, t.h);
    } else {
        std::vector<uint8_t> flip(t.px8.size());
        size_t row = static_cast<size_t>(t.w) * 4;
        for (int y = 0; y < t.h; ++y) memcpy(&flip[y * row], &t.px8[(t.h - 1 - y) * row], row);
        png = Png8(flip.data(), t.w, t.h, 4);
    }
    std::vector<uint8_t>().swap(t.px8);
    std::vector<uint16_t>().swap(t.px16);
    return png;
}
}  // namespace

bool WriteMcap(McapData& j, const std::wstring& path, McapResult* res, std::string* err) {
    McapResult out;
    std::wstring tmp = path + L".tmp";
    Zip z;
    if (!z.Open(tmp)) {
        if (err) *err = "cannot create the capture file";
        return false;
    }
    std::unordered_map<uint32_t, std::string> arbToProg;
    for (const McapProgram& p : j.progs)
        if (p.arbName) arbToProg[p.arbName] = p.file + ":" + p.entry;

    std::string calls, events;
    calls.reserve(j.recs.size() * 200);
    size_t m = 0;
    uint32_t lastFrame = 0;
    int lastPass = -1;
    char b[320];
    auto event = [&](size_t at, const std::string& body) {
        events += "{\"at\":" + std::to_string(at) + "," + body + "}\n";
        ++out.events;
    };
    for (size_t i = 0; i < j.recs.size(); ++i) {
        const hub::Rec& r = j.recs[i];
        uint32_t pos = i < j.ringPos.size() ? j.ringPos[i] : static_cast<uint32_t>(i);
        while (m < j.markers.size() && static_cast<int32_t>(j.markers[m].ringPos - pos) <= 0) event(i, j.markers[m++].json);
        if (r.frame != lastFrame) {
            if (lastFrame) {
                snprintf(b, sizeof b, "\"type\":\"frame-end\",\"frame\":%u", lastFrame);
                event(i, b);
            }
            snprintf(b, sizeof b, "\"type\":\"frame-begin\",\"frame\":%u", r.frame);
            event(i, b);
            lastFrame = r.frame;
            lastPass = -1;
        }
        if (r.pass != lastPass) {
            snprintf(b, sizeof b, "\"type\":\"pass\",\"pass\":%u,\"frame\":%u", r.pass, r.frame);
            event(i, b);
            lastPass = r.pass;
        }
        const std::string& fn = r.fn < j.fnName.size() ? j.fnName[r.fn] : std::string();
        hub::Src src = r.fn < j.fnSrc.size() ? j.fnSrc[r.fn] : hub::Src::ExeIat;
        if (Categorize(fn.c_str()) & kDraw) ++out.draws;
        if (fn == "glBindProgramARB") {
            auto it = arbToProg.find(r.a[1]);
            snprintf(b, sizeof b, "\"type\":\"cg-bind\",\"target\":%u,\"arb\":%u,\"program\":\"%s\",\"frame\":%u", r.a[0], r.a[1],
                     it != arbToProg.end() ? jsonmini::Escape(it->second).c_str() : "", r.frame);
            event(i, b);
        }
        std::string payload;
        auto p = j.pay.find(pos);
        if (p != j.pay.end() && fn == p->second.sig->name) {
            const McapPayload& pr = p->second;
            if (pr.hashOnly) payload = "{\"bytes\":" + std::to_string(pr.full) + ",\"hash\":\"" + pr.hash + "\"}";
            else payload = PayloadJson(pr.sig->elem, reinterpret_cast<const uint8_t*>(j.arena.data()) + pr.off, pr.len);
            if (pr.capped) payload = "{\"data\":" + payload + ",\"capped\":true}";
            ++out.payloads;
        }
        calls += CallJson(i, r, fn.empty() ? "?" : fn.c_str(), src, payload.empty() ? nullptr : &payload);
        calls += '\n';
    }
    for (; m < j.markers.size(); ++m) event(j.recs.size(), j.markers[m].json);
    if (lastFrame) {
        snprintf(b, sizeof b, "\"type\":\"frame-end\",\"frame\":%u", lastFrame);
        event(j.recs.size(), b);
    }
    for (const std::string& n : j.notes) event(j.recs.size(), "\"type\":\"note\",\"text\":\"" + jsonmini::Escape(n) + "\"");
    z.Add("calls.jsonl", calls);
    std::string().swap(calls);
    z.Add("events.jsonl", events);
    z.Add("state/begin.json", j.begin);
    z.Add("state/end.json", j.end);

    jsonmini::Arr progIndex;
    for (size_t k = 0; k < j.progs.size(); ++k) {
        const McapProgram& p = j.progs[k];
        jsonmini::Obj o;
        std::string asmFile = p.asmText.empty() ? "" : "programs/" + std::to_string(k) + ".asm";
        o.Int("n", static_cast<long long>(k)).Str("file", p.file).Str("entry", p.entry).Str("stage", StageName(p.stage))
            .UInt("cgProgram", p.cgProgram).UInt("arbName", p.arbName).UInt("binds", p.binds).Str("source", p.source)
            .Bool("failed", p.failed).Bool("overridden", p.overridden).Bool("glsl", p.glsl).Str("owner", p.owner)
            .Str("asm", asmFile);
        progIndex.Raw(o.End());
        if (!asmFile.empty()) {
            z.Add(asmFile, p.asmText);
            ++out.programsWithAsm;
        }
    }
    if (j.opt.shaders) {
        jsonmini::Obj root;
        root.Str("format", "melange-capture").Int("version", 1).Raw("programs", progIndex.End());
        z.Add("programs/index.json", root.End());
    }

    jsonmini::Arr texIndex;
    for (McapTexture& t : j.tex) {
        std::string file;
        if (!t.px8.empty() || !t.px16.empty()) {
            std::string png = FlipPng(t);
            if (!png.empty()) {
                file = "textures/" + std::to_string(t.gl) + ".png";
                z.Add(file, png, false);
                ++out.textures;
            }
        }
        jsonmini::Obj o;
        o.UInt("gl", t.gl).Int("w", t.w).Int("h", t.h).Int("internalFormat", t.ifmt).Int("levels", t.levels)
            .Bool("depth", t.depth).Str("name", t.name).Str("file", file);
        if (!t.skipped.empty()) o.Str("skipped", t.skipped);
        texIndex.Raw(o.End());
    }
    if (j.opt.textures) {
        jsonmini::Obj root;
        root.Str("format", "melange-capture").Int("version", 1).Raw("textures", texIndex.End());
        z.Add("textures/index.json", root.End());
    }
    if (!j.frame.empty()) z.Add("frame.png", Png8(j.frame.data(), j.frameW, j.frameH, 4), false);

    out.calls = j.recs.size();
    out.programs = j.progs.size();
    jsonmini::Arr files;
    for (const std::string& f : z.files) files.Str(f);
    files.Str("manifest.json");
    jsonmini::Obj counts;
    counts.UInt("calls", out.calls).UInt("draws", out.draws).UInt("textures", out.textures).UInt("programs", out.programs)
        .UInt("programsWithAsm", out.programsWithAsm).UInt("payloads", out.payloads).UInt("events", out.events);
    jsonmini::Obj opts;
    opts.UInt("frames", j.opt.frames).Bool("textures", j.opt.textures).Bool("shaders", j.opt.shaders)
        .Bool("frameImage", j.opt.frameImage).Bool("bufferSizes", j.opt.bufferSizes).UInt("maxTextureMB", j.opt.maxTextureMB);
    jsonmini::Arr frames;
    for (uint64_t f = j.firstFrame; f && f <= j.lastFrame; ++f) frames.Raw(std::to_string(f));
    jsonmini::Obj gl;
    gl.Str("vendor", j.glVendor).Str("renderer", j.glRenderer).Str("version", j.glVersion);
    jsonmini::Obj win;
    win.Int("w", j.winW).Int("h", j.winH);
    jsonmini::Arr notes;
    for (const std::string& n : j.notes) notes.Str(n);
    jsonmini::Obj man;
    man.Str("format", "melange-capture").Int("version", 1).Str("melangeVersion", j.melangeVersion).Str("exeSha256", j.exeSha256)
        .Str("exeBuild", j.exeBuild).Raw("gl", gl.End()).Raw("window", win.End()).Raw("frames", frames.End())
        .Str("scene", j.scene).Raw("counts", counts.End()).Raw("options", opts.End()).Raw("files", files.End())
        .UInt("droppedRecords", j.dropped).UInt("droppedPayloads", j.droppedPayloads)
        .Int("readbackMs", static_cast<long long>(j.readbackMs + 0.5)).Raw("notes", notes.End());
    z.Add("manifest.json", man.End());
    if (!z.Close()) {
        _wremove(tmp.c_str());
        if (err) *err = "zip write failed";
        return false;
    }
    _wremove(path.c_str());
    if (_wrename(tmp.c_str(), path.c_str()) != 0) {
        if (err) *err = "rename failed";
        return false;
    }
    if (res) *res = out;
    return true;
}
}  // namespace melange::mirage::trace
