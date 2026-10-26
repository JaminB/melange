// User gestures for consent checks (gesture.h). No game dependency: the sandbox self-test links this file as is.
#include "input/gesture.h"

#include <windows.h>

#include <map>
#include <string>

namespace melange::input::gesture {
namespace {
struct Last {
    uint64_t any = 0, consent = 0;  // QPC ms of the last gesture, and of the last consent-grade one
};
std::map<std::string, Last> g_last;

uint64_t NowMs() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart) * 1000 / freq;
}
}  // namespace

void Mark(const char* modId, bool consentGrade) {
    if (!modId || !*modId) return;
    Last& l = g_last[modId];
    l.any = NowMs();
    if (consentGrade) l.consent = l.any;
}

bool Recent(const char* modId, uint32_t ms, bool needConsentGrade) {
    if (!modId) return false;
    auto it = g_last.find(modId);
    if (it == g_last.end()) return false;
    const uint64_t at = needConsentGrade ? it->second.consent : it->second.any;
    return at != 0 && NowMs() - at <= ms;
}

void Clear(const char* modId) {
    if (modId) g_last.erase(modId);
}
}  // namespace melange::input::gesture
