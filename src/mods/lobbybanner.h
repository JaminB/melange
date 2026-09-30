#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The lobby banner stack: every component that explains a held or changed lobby start draws through one panel,
// stacked below each other at the top right of the lobby screen, so banners never overlap. Main thread.
namespace melange::mods::lobbybanner {
int Add(const char* owner, int order);                  // stack slot; lower order draws first
void Set(int slot, uint32_t color, std::string_view title, const std::vector<std::string>& lines);   // "" hides
void Remove(int slot);

// Layout (pure): the lines a text wraps into at `chars` characters per line.
std::vector<std::string> Wrap(std::string_view text, size_t chars);
}  // namespace melange::mods::lobbybanner
