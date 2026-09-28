// Thumper: mod discovery, spice.json manifests, load order, enable/disable (scaffold stub).
#include "core/module.h"
#include "melange/mods.h"

namespace melange::mods {
int List(ModInfo*, int) { return 0; }
bool Find(const char*, ModInfo*) { return false; }
bool SetEnabled(const char*, bool) { return false; }
bool SetDeepDesert(const char*, bool) { return false; }
const wchar_t* ModsDir() { return L""; }
int OnChange(ChangeFn, void*) { return 0; }
void RemoveOnChange(int) {}
}  // namespace melange::mods

namespace {
class Thumper final : public melange::Module {
public:
    const char* Name() const override { return "Thumper"; }
    const char* Description() const override { return "mod manager: spice.json manifests, load order, Mods page"; }
    int Order() const override { return 36; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(Thumper);
