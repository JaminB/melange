// Module "MirageShaders": Cg overrides, hot reload, GLSL replacement, FXAA fix (stub).
#include "core/module.h"
#include "melange/shaders.h"

namespace {
class MirageShaders final : public melange::Module {
public:
    const char* Name() const override { return "MirageShaders"; }
    const char* Description() const override { return "shader overrides, hot reload, GLSL replacement, FXAA fix"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 43; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(MirageShaders);

namespace melange::shaders {
size_t ListPrograms(ProgramInfo*, size_t) { return 0; }
const char* Profile(uint8_t) { return ""; }
int Reload(const char*) { return 0; }
bool SetParam(const char*, const char*, const char*, const float*, int) { return false; }
bool GetParam(const char*, const char*, const char*, float*, int) { return false; }
int AddOverrideRoot(const wchar_t*, const char*) { return 0; }
void RemoveOverrideRoot(int) {}
size_t LastErrors(CompileError*, size_t) { return 0; }
Stats GetStats() { return {}; }
}  // namespace melange::shaders
