// SimTweak: in-memory weapon data overrides from sim mods (scaffold stub).
#include "core/module.h"

namespace {
class SimTweak final : public melange::Module {
public:
    const char* Name() const override { return "SimTweak"; }
    const char* Description() const override { return "weapon data tweaks from sim mods"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 53; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(SimTweak);
