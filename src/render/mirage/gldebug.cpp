// Module "MirageDebug": optional KHR_debug context (stub).
#include "core/module.h"
#include "melange/gldebug.h"

namespace {
class MirageDebug final : public melange::Module {
public:
    const char* Name() const override { return "MirageDebug"; }
    const char* Description() const override { return "optional GL debug context, KHR_debug messages to the logs"; }
    bool DefaultEnabled() const override { return false; }
    int Order() const override { return 42; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(MirageDebug);

namespace melange::gldebug {
bool DebugContext() { return false; }
Stats GetStats() { return {}; }
}  // namespace melange::gldebug
