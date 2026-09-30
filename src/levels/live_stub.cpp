// Live pack changes at the menu: not available in this build.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "melange/levels.h"

namespace melange::levels {
namespace {
struct Observer {
    int handle;
    PacksChangedFn fn;
    void* user;
};
std::vector<Observer> g_obs;
int g_next = 1;

bool Refuse(char* err, size_t errLen) {
    if (err && errLen) snprintf(err, errLen, "live pack changes are not available in this build; restart the game");
    return false;
}
}  // namespace

bool EnablePackLive(const char*, char* err, size_t errLen) { return Refuse(err, errLen); }
bool DisablePackLive(const char*, char* err, size_t errLen) { return Refuse(err, errLen); }

int OnPacksChanged(PacksChangedFn fn, void* user) {
    if (!fn) return 0;
    g_obs.push_back({g_next, fn, user});
    return g_next++;
}

void RemoveOnPacksChanged(int handle) {
    std::erase_if(g_obs, [handle](const Observer& o) { return o.handle == handle; });
}
}  // namespace melange::levels
