// Module "MiragePostFX": post-processing effect stack (stub).
#include "core/module.h"
#include "melange/postfx.h"

namespace {
class MiragePostFX final : public melange::Module {
public:
    const char* Name() const override { return "MiragePostFX"; }
    const char* Description() const override { return "post-FX stack at the PostWorld and Final stages"; }
    int Order() const override { return 44; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(MiragePostFX);

namespace melange::postfx {
size_t ListEffects(EffectInfo*, size_t) { return 0; }
bool SetEnabled(const char*, bool) { return false; }
bool SetOrder(const char*, int) { return false; }
bool SetParam(const char*, const char*, const float*, int) { return false; }
bool GetParam(const char*, const char*, float*, int) { return false; }
int Reload(const char*) { return 0; }
int AddCodePass(const char*, render::Stage, int, PassFn, void*) { return 0; }
void RemoveCodePass(int) {}
Stats GetStats() { return {}; }
}  // namespace melange::postfx
