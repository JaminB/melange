#pragma once
#include <vector>

#include "core/config.h"

// A module is one self-contained fix or feature: a .cpp anywhere under src/ that derives from melange::Module
// and ends with MELANGE_MODULE(YourClass). It gets its own [Name] section in Melange.ini with at least "Enabled".
namespace melange {
class Module {
public:
    virtual ~Module() = default;
    virtual const char* Name() const = 0;
    virtual const char* Description() const = 0;
    virtual bool DefaultEnabled() const { return true; }
    // Modules that patch hard-coded addresses must return true: they are skipped on unknown exe builds.
    virtual bool RequiresKnownBuild() const { return false; }
    // Lower runs first (diagnostics should come before everything else).
    virtual int Order() const { return 100; }
    virtual bool Install() = 0;
    virtual void Uninstall() {}

protected:
    // Per-module settings; the default is written to the ini the first time so every option is discoverable.
    int Int(const char* key, int def) const;
    bool Bool(const char* key, bool def) const;
    float Float(const char* key, float def) const;
};

namespace modules {
using Factory = Module* (*)();
bool Register(Factory f);
void InstallAll();
void UninstallAll();
const std::vector<Module*>& Installed();
}  // namespace modules
}  // namespace melange

#define MELANGE_MODULE(T) \
    static const bool melange_registered_##T = ::melange::modules::Register([]() -> ::melange::Module* { return new T(); })
