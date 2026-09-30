#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "erg/patch.h"

// The level surround (.hmp): 100x100 f32 heights 0..1 then 100x100 u8 blend, row-major.
namespace melange::erg::hmp {
struct Surround {
    std::vector<float> heights = std::vector<float>(kHmpCells, 0.0f);
    std::vector<uint8_t> blend = std::vector<uint8_t>(kHmpCells, 0);
};
bool Read(const std::vector<uint8_t>& bytes, Surround* out, std::string* err);
std::vector<uint8_t> Write(const Surround& s);
// Applies the runs in order; on failure `s` is left unchanged.
bool ApplyRuns(Surround& s, const std::vector<HmpRun>& heights, const std::vector<HmpRun>& blend, std::string* err);
// Changed cells as runs (the editor's cellRuns): against `base`, or against zeros when there is none.
void Runs(const Surround* base, const Surround& edited, std::vector<HmpRun>* heights, std::vector<HmpRun>* blend);
}  // namespace melange::erg::hmp
