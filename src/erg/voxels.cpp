#include "erg/voxels.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "erg/names.h"

namespace melange::erg::voxels {
namespace {
bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

bool StemShape(std::string_view s) {
    if (s.empty() || s.size() > names::kMaxStem) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool SafeRoot(const std::string& root) {
    const std::filesystem::path p(root);
    if (root.empty() || p.has_root_name() || p.has_root_directory()) return false;
    bool first = true;
    for (auto& part : p) {
        std::string s = part.string();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == ".." || (first && (s == "data" || s == "data2"))) return false;
        first = false;
    }
    return true;
}
}  // namespace

bool ValidEdit(uint32_t base, uint32_t now) {
    if (!ValidRunValue(now)) return false;
    const uint32_t kept = now & kKeptMask;
    return kept == 0 || kept == (base & kKeptMask);
}

size_t Cells(const Frame& f) {
    return static_cast<size_t>(std::max(f.size[0], 0)) * std::max(f.size[1], 0) * std::max(f.size[2], 0);
}

bool Decode(const std::vector<uint8_t>& le, std::vector<uint32_t>* out, std::string* err) {
    if (le.size() % 4) return Fail(err, "a voxel blob is not a whole number of words");
    out->resize(le.size() / 4);
    for (size_t i = 0; i < out->size(); ++i)
        (*out)[i] = le[4 * i] | (le[4 * i + 1] << 8) | (le[4 * i + 2] << 16) | (static_cast<uint32_t>(le[4 * i + 3]) << 24);
    return true;
}

std::vector<uint8_t> Encode(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> out(words.size() * 4);
    for (size_t i = 0; i < words.size(); ++i)
        for (int b = 0; b < 4; ++b) out[4 * i + b] = static_cast<uint8_t>(words[i] >> (8 * b));
    return out;
}

bool ApplyOps(const Patch& p, const Scene& base, FrameWords& words, std::string* err, Stats* stats) {
    FrameWords work;                                   // copies of the frames the patch touches
    std::map<int64_t, const std::vector<uint32_t>*> original;
    Stats st;
    for (size_t i = 0; i < p.ops.size(); ++i) {
        const Op& op = p.ops[i];
        if (op.kind != Op::Kind::Voxels) continue;
        const std::string path = "ops[" + std::to_string(i) + "]";
        const Frame* f = base.FindFrame(op.frame);
        if (!f) return Fail(err, path + ".frame: " + std::to_string(op.frame) + " is not a frame of the base");
        if (f->voxels < 0) return Fail(err, path + ".frame: " + std::to_string(op.frame) + " has no voxels");
        auto src = words.find(op.frame);
        if (src == words.end() || src->second.size() != Cells(*f))
            return Fail(err, path + ".frame: the voxels of frame " + std::to_string(op.frame) + " are not loaded");
        auto [it, fresh] = work.try_emplace(op.frame, src->second);
        if (fresh) {
            original[op.frame] = &src->second;
            ++st.frames;
        }
        std::vector<uint32_t>& w = it->second;
        const std::vector<uint32_t>& orig = *original[op.frame];
        for (size_t r = 0; r < op.runs.size(); ++r) {
            const VoxelRun& run = op.runs[r];
            const std::string rp = path + ".runs[" + std::to_string(r) + "]";
            if (static_cast<uint64_t>(run.start) + run.count > w.size())
                return Fail(err, rp + ": past the frame's " + std::to_string(w.size()) + " voxels");
            for (uint32_t j = run.start; j < run.start + run.count; ++j)
                if (!ValidEdit(orig[j], run.value))
                    return Fail(err, rp + ": voxel " + std::to_string(j) +
                                         " may only be carved, filled with a plain material or painted");
            std::fill_n(w.begin() + run.start, run.count, run.value);
            ++st.runs;
        }
    }
    for (auto& [frame, w] : work) {
        const std::vector<uint32_t>& orig = *original[frame];
        for (size_t j = 0; j < w.size(); ++j) {
            if (w[j] == orig[j]) continue;
            ++st.changed;
            if (Solid(orig[j]) && !Solid(w[j])) ++st.carved;
            else if (!Solid(orig[j]) && Solid(w[j])) ++st.filled;
            else if (Solid(w[j]) && Material(w[j]) != Material(orig[j])) ++st.painted;
        }
    }
    for (auto& [frame, w] : work) words[frame] = std::move(w);
    if (stats) *stats = st;
    return true;
}

bool ApplyToBlobs(const Patch& p, const Scene& base, std::map<int64_t, std::vector<uint8_t>>& blobs, std::string* err,
                  Stats* stats) {
    FrameWords words;
    for (auto& op : p.ops) {
        if (op.kind != Op::Kind::Voxels || words.count(op.frame)) continue;
        const Frame* f = base.FindFrame(op.frame);
        if (!f || f->voxels < 0) continue;              // ApplyOps names the op
        auto b = blobs.find(f->voxels);
        if (b == blobs.end()) continue;
        if (!Decode(b->second, &words[op.frame], err)) return false;
    }
    if (!ApplyOps(p, base, words, err, stats)) return false;
    for (auto& [frame, w] : words) blobs[base.FindFrame(frame)->voxels] = Encode(w);
    return true;
}

std::vector<VoxelRun> Runs(const std::vector<uint32_t>& before, const std::vector<uint32_t>& after) {
    std::vector<VoxelRun> runs;
    const size_t n = std::min(before.size(), after.size());
    for (size_t i = 0; i < n; ++i) {
        if (before[i] == after[i]) continue;
        if (!runs.empty() && runs.back().start + runs.back().count == i && runs.back().value == after[i])
            ++runs.back().count;
        else
            runs.push_back({static_cast<uint32_t>(i), 1, after[i]});
    }
    return runs;
}

bool DiffOps(const Scene& base, const FrameWords& before, const FrameWords& after, std::vector<Op>* out, std::string* err) {
    std::vector<Op> ops;
    for (auto& f : base.frames) {
        if (f.voxels < 0) continue;
        auto b = before.find(f.id), a = after.find(f.id);
        if (b == before.end() || a == after.end()) continue;
        if (b->second.size() != Cells(f) || a->second.size() != Cells(f))
            return Fail(err, "frame " + std::to_string(f.id) + ": the voxel arrays do not match the frame size");
        Op op;
        op.kind = Op::Kind::Voxels;
        op.frame = f.id;
        op.runs = Runs(b->second, a->second);
        if (op.runs.empty()) continue;
        if (op.runs.size() > kMaxRunsPerFrame)
            return Fail(err, "frame " + std::to_string(f.id) + ": " + std::to_string(op.runs.size()) + " runs, at most 2000");
        for (auto& r : op.runs)
            for (uint32_t j = r.start; j < r.start + r.count; ++j)
                if (!ValidEdit(b->second[j], r.value))
                    return Fail(err, "frame " + std::to_string(f.id) + ": voxel " + std::to_string(j) + " is not a carve, fill or paint");
        ops.push_back(std::move(op));
    }
    *out = std::move(ops);
    return true;
}

bool TerrainChanged(const Patch& p) {
    return std::any_of(p.ops.begin(), p.ops.end(), [](const Op& op) { return op.kind == Op::Kind::Voxels || op.kind == Op::Kind::Hmp; });
}

std::vector<std::string> ShadowFiles(std::string_view stem) {
    std::vector<std::string> out;
    for (const char* tod : {"DAY", "EVENING", "NIGHT"}) out.push_back("Maps/" + std::string(stem) + tod + ".csh");
    return out;
}

size_t DeleteShadows(const std::filesystem::path& game, const std::vector<std::string>& roots, std::string_view stem,
                     std::string* err) {
    if (!StemShape(stem)) {
        Fail(err, "not a level stem");
        return 0;
    }
    size_t n = 0;
    for (auto& root : roots) {
        if (!SafeRoot(root)) {
            Fail(err, "refused the root '" + root + "'");
            continue;
        }
        for (auto& rel : ShadowFiles(stem)) {
            std::error_code ec;
            const std::filesystem::path file = game / root / rel;
            if (!std::filesystem::is_regular_file(file, ec)) continue;
            if (std::filesystem::remove(file, ec)) ++n;
            else if (ec) Fail(err, file.string() + ": " + ec.message());
        }
    }
    return n;
}
}  // namespace melange::erg::voxels
