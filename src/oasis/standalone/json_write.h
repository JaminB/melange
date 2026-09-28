#pragma once
#include <string>

#include "tools/json_read.h"

// Round-trips a parsed json::Value back to text, so a small file (thumper-state.json) can be read, have one
// field changed, and be written back without hand-modelling every other field.
namespace melange::oasis::standalone {
std::string WriteJson(const json::Value& v);
}
