// Test-command registry (melange/testcmd.h): modules register verbs that an automation driver can invoke.
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
        LOG_WARN("[testcmd] verb '%s' already registered, ignoring", verb);
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
    // A known verb whose handler fails is still "handled", so the caller doesn't report it as unknown.
    if (!e.fn(args, e.user)) LOG_WARN("[auto] %.*s failed", static_cast<int>(verb.size()), verb.data());
    return true;
}
}  // namespace melange::testcmd
