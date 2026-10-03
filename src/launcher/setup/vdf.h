#pragma once
#include <string>
#include <string_view>
#include <vector>

// Valve KeyValues text (libraryfolders.vdf, appmanifest_*.acf): tolerant, never throws.
namespace melange::launcher::vdf {
constexpr size_t kMaxBytes = 1 << 20;
constexpr int kMaxDepth = 16;

struct Node {
    std::string key, value;       // value: a leaf
    std::vector<Node> children;   // a block
    bool block = false;
    const Node* Get(std::string_view k) const;   // case-insensitive, first match
    std::string Str(std::string_view k) const;   // a leaf child's value or ""
};
// On a syntax error or a limit, `out` holds what was parsed so far and the result is false.
bool Parse(std::string_view text, Node* out);

// Library roots from libraryfolders.vdf, both the current and the old shape; ones listing `appId` first.
std::vector<std::string> Libraries(const Node& root, std::string_view appId);
// "installdir" of an appmanifest_<id>.acf.
std::string InstallDir(const Node& root);
}  // namespace melange::launcher::vdf
