#include "erg/hmp.h"

#include <cmath>
#include <cstring>

namespace melange::erg::hmp {
namespace {
bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

bool CheckRuns(const std::vector<HmpRun>& runs, const char* what, bool blend, std::string* err) {
    for (size_t k = 0; k < runs.size(); ++k) {
        const HmpRun& r = runs[k];
        const std::string at = std::string(what) + "[" + std::to_string(k) + "]";
        if (r.count == 0 || r.start >= kHmpCells || r.count > kHmpCells - r.start) return Fail(err, at + ": the run is empty or past the 10000 cells");
        if (blend ? !(r.value >= 0 && r.value <= 255 && r.value == std::floor(r.value)) : !(r.value >= 0 && r.value <= 1))
            return Fail(err, at + (blend ? ": a blend value is an integer 0..255" : ": a height is 0..1"));
    }
    return true;
}

template <class T, class Out>
void CellRuns(const std::vector<T>* before, const std::vector<T>& after, Out* out) {
    for (size_t i = 0; i < after.size(); ++i) {
        if (before ? (*before)[i] == after[i] : after[i] == 0) continue;
        if (!out->empty() && out->back().start + out->back().count == i && out->back().value == double(after[i])) ++out->back().count;
        else out->push_back({static_cast<uint32_t>(i), 1, double(after[i])});
    }
}
}  // namespace

bool Read(const std::vector<uint8_t>& bytes, Surround* out, std::string* err) {
    if (bytes.size() != kHmpBytes) return Fail(err, ".hmp: " + std::to_string(bytes.size()) + " bytes, expected 50000");
    Surround s;
    std::memcpy(s.heights.data(), bytes.data(), kHmpCells * 4);
    std::memcpy(s.blend.data(), bytes.data() + kHmpCells * 4, kHmpCells);
    *out = std::move(s);
    return true;
}

std::vector<uint8_t> Write(const Surround& s) {
    std::vector<uint8_t> out(kHmpBytes, 0);
    if (s.heights.size() == kHmpCells) std::memcpy(out.data(), s.heights.data(), kHmpCells * 4);
    if (s.blend.size() == kHmpCells) std::memcpy(out.data() + kHmpCells * 4, s.blend.data(), kHmpCells);
    return out;
}

bool ApplyRuns(Surround& s, const std::vector<HmpRun>& heights, const std::vector<HmpRun>& blend, std::string* err) {
    if (s.heights.size() != kHmpCells || s.blend.size() != kHmpCells) return Fail(err, "the surround is not 100x100");
    if (!CheckRuns(heights, "heights", false, err) || !CheckRuns(blend, "blend", true, err)) return false;
    for (const HmpRun& r : heights) std::fill_n(s.heights.begin() + r.start, r.count, static_cast<float>(r.value));
    for (const HmpRun& r : blend) std::fill_n(s.blend.begin() + r.start, r.count, static_cast<uint8_t>(r.value));
    return true;
}

void Runs(const Surround* base, const Surround& edited, std::vector<HmpRun>* heights, std::vector<HmpRun>* blend) {
    heights->clear();
    blend->clear();
    CellRuns(base ? &base->heights : nullptr, edited.heights, heights);
    CellRuns(base ? &base->blend : nullptr, edited.blend, blend);
}
}  // namespace melange::erg::hmp
