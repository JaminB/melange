// The body of every detached std::thread. An exception that leaves a std::thread's function calls std::terminate and
// so abort(), which ends the game; a worker that fails should log the failure and stop instead.
#pragma once
#include <exception>

#include "core/log.h"

namespace melange {
template <class F>
void GuardedThreadBody(const char* name, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        LOG_ERROR("[%s] thread failed: %s", name, e.what());
    } catch (...) {
        LOG_ERROR("[%s] thread failed: unknown exception", name);
    }
}
}  // namespace melange
