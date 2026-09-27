#include "core/module.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "core/game.h"
#include "core/log.h"

namespace wf {
int Module::Int(const char* key, int def) const {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", def);
    config::EnsureKey(Name(), key, buf);
    return config::GetInt(Name(), key, def);
}

bool Module::Bool(const char* key, bool def) const { return Int(key, def ? 1 : 0) != 0; }

float Module::Float(const char* key, float def) const {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", def);
    config::EnsureKey(Name(), key, buf);
    return config::GetFloat(Name(), key, def);
}

namespace modules {
namespace {
// Function-local statics: registration happens during static init, in unspecified order.
std::vector<Factory>& Factories() {
    static std::vector<Factory> f;
    return f;
}
std::vector<Module*>& InstalledList() {
    static std::vector<Module*> m;
    return m;
}

// A module that faults while installing must not take the game down with it.
bool SafeInstall(Module* m) {
    __try {
        return m->Install();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WF_ERROR("module %s raised exception 0x%08lx during install", m->Name(), GetExceptionCode());
        return false;
    }
}
}  // namespace

bool Register(Factory f) {
    Factories().push_back(f);
    return true;
}

void InstallAll() {
    std::vector<Module*> all;
    for (auto f : Factories()) all.push_back(f());
    std::stable_sort(all.begin(), all.end(), [](Module* a, Module* b) { return a->Order() < b->Order(); });

    for (Module* m : all) {
        config::EnsureKey(m->Name(), "Enabled", m->DefaultEnabled() ? "1" : "0");
        if (!config::GetBool(m->Name(), "Enabled", m->DefaultEnabled())) {
            WF_INFO("module %-16s disabled in ini", m->Name());
            delete m;
            continue;
        }
        if (m->RequiresKnownBuild() && !game::IsKnownBuild()) {
            WF_WARN("module %-16s skipped: needs exe build #1077, this exe is unrecognised", m->Name());
            delete m;
            continue;
        }
        if (SafeInstall(m)) {
            WF_INFO("module %-16s installed  - %s", m->Name(), m->Description());
            InstalledList().push_back(m);
        } else {
            WF_ERROR("module %-16s FAILED to install", m->Name());
            delete m;
        }
    }
}

void UninstallAll() {
    auto& list = InstalledList();
    for (auto it = list.rbegin(); it != list.rend(); ++it) (*it)->Uninstall();
}

const std::vector<Module*>& Installed() { return InstalledList(); }
}  // namespace modules
}  // namespace wf
