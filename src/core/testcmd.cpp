// Automation test-command registry (melange/testcmd.h): lets any module register verbs that
// src/tools/automation.cpp's command file can invoke, without automation.cpp knowing about them.
#include "melange/testcmd.h"

#include <mutex>
#include <unordered_map>

#include "core/log.h"

namespace melange::testcmd {
namespace {
struct Entry {
    Handler fn;
    void* user;
};

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::mutex& Mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, Entry>& Table() {
    static std::unordered_map<std::string, Entry> t;
    return t;
}
}  // namespace

bool Register(const char* verb, Handler fn, void* user) {
    if (!verb || !*verb || !fn) return false;
    std::string key = Lower(verb);
    std::lock_guard lk(Mutex());
    auto [it, inserted] = Table().try_emplace(key, Entry{fn, user});
    if (!inserted) {
        WF_WARN("[testcmd] verb '%s' already registered, ignoring", verb);
        return false;
    }
    return true;
}

bool Dispatch(std::string_view verb, std::string_view args) {
    Entry e;
    {
        std::lock_guard lk(Mutex());
        auto it = Table().find(Lower(verb));
        if (it == Table().end()) return false;  // no handler exists: let the caller warn
        e = it->second;
    }
    // The verb is recognised either way; a failing handler logs its own warning rather than
    // making the caller treat a known verb as "unknown command".
    if (!e.fn(args, e.user)) WF_WARN("[auto] %.*s failed", static_cast<int>(verb.size()), verb.data());
    return true;
}
}  // namespace melange::testcmd
