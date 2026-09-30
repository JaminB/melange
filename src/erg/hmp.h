#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "erg/patch.h"

// The level surround (.hmp): 100x100 f32 heights 0..1 then 100x100 u8 blend, row-major.
namespace melange::erg::hmp {
struct Surround {
    std::vector<float> heights;                        // kHmpCells
    std::vector<uint8_t> blend;                        // kHmpCells
};
bool Read(const std::vector<uint8_t>& bytes, Surround* out, std::string* err);
std::vector<uint8_t> Write(const Surround& s);
bool ApplyRuns(Surround& s, const std::vector<HmpRun>& heights, const std::vector<HmpRun>& blend, std::string* err);
}  // namespace melange::erg::hmp
