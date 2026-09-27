#include "core/jlog_bus_filter.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace wf::jlog::busfilter {
namespace {
std::mutex g_mx;
std::unordered_set<std::string> g_deny;
std::unordered_set<std::string> g_allow;

void Split(std::string_view csv, std::unordered_set<std::string>& out) {
    size_t pos = 0;
    while (pos < csv.size()) {
        size_t comma = csv.find(',', pos);
        std::string_view part = csv.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        pos = comma == std::string_view::npos ? csv.size() : comma + 1;
        size_t a = part.find_first_not_of(' ');
        size_t b = part.find_last_not_of(' ');
        if (a == std::string_view::npos) continue;
        out.emplace(part.substr(a, b - a + 1));
    }
}
}  // namespace

void Init(std::string_view denyList, std::string_view allowList) {
    std::lock_guard lk(g_mx);
    g_deny.clear();
    g_allow.clear();
    Split(denyList, g_deny);
    Split(allowList, g_allow);
}

bool ShouldLog(std::string_view name) {
    std::string key(name);
    std::lock_guard lk(g_mx);
    if (g_allow.count(key)) return true;
    if (g_deny.count(key)) return false;
    return true;
}

bool IsAllowed(std::string_view name) {
    std::lock_guard lk(g_mx);
    return g_allow.count(std::string(name)) != 0;
}
bool IsDenied(std::string_view name) {
    std::lock_guard lk(g_mx);
    return g_deny.count(std::string(name)) != 0;
}
void SetAllow(std::string_view name, bool on) {
    std::lock_guard lk(g_mx);
    if (on)
        g_allow.emplace(name);
    else
        g_allow.erase(std::string(name));
}
void SetDeny(std::string_view name, bool on) {
    std::lock_guard lk(g_mx);
    if (on)
        g_deny.emplace(name);
    else
        g_deny.erase(std::string(name));
}

}  // namespace wf::jlog::busfilter
