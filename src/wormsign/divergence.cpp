#include "wormsign/divergence.h"

#include <mutex>
#include <vector>

namespace melange::wormsign {
namespace {
struct Obs {
    int handle;
    DivergenceFn fn;
    void* user;
};
std::mutex g_mu;
std::vector<Obs> g_obs;
int g_next = 1;
}  // namespace

int OnDivergence(DivergenceFn fn, void* user) {
    if (!fn) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    g_obs.push_back(Obs{g_next, fn, user});
    return g_next++;
}

void RemoveOnDivergence(int handle) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (size_t i = 0; i < g_obs.size(); ++i)
        if (g_obs[i].handle == handle) {
            g_obs.erase(g_obs.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

namespace divergence {
void Raise(const Divergence& d) {
    std::vector<Obs> v;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        v = g_obs;
    }
    for (const Obs& o : v) o.fn(d, o.user);
}
}  // namespace divergence
}  // namespace melange::wormsign
