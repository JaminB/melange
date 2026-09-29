#include "assets/searchpath.h"

#include <cctype>

#include "core/log.h"
#include "weapons/engine.h"

namespace melange::assets::searchpath {
namespace {
std::vector<std::string> g_added;

std::string Norm(const char* s) {
    std::string n;
    for (; *s; ++s) n.push_back(*s == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(*s))));
    while (!n.empty() && n.back() == '/') n.pop_back();
    return n;
}
}  // namespace

bool Add(const char* gameRelDir) {
    if (!gameRelDir || !*gameRelDir) return false;
    const std::string n = Norm(gameRelDir);
    for (auto& a : g_added)
        if (a == n) return true;
    if (!weapons::engine::AddSearchPath(gameRelDir)) {
        LOG_ERROR("[assets] adding the search path '%s' failed", gameRelDir);
        return false;
    }
    g_added.push_back(n);
    LOG_INFO("[assets] search path added: %s", gameRelDir);
    return true;
}

std::vector<std::string> Added() { return g_added; }
}  // namespace melange::assets::searchpath
