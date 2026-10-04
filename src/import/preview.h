#pragma once
#include <cstdint>
#include <vector>

namespace melange::import {
// A TGA preview scaled down to at most `width` pixels wide (box filter), as PNG. False when it does not decode.
bool TgaToPng(const std::vector<uint8_t>& tga, int width, std::vector<uint8_t>* png);
}  // namespace melange::import
