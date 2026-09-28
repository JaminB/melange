// Melange entry point. Loaded by Ultimate ASI Loader (dinput8.dll) as melange.asi.
#include <windows.h>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "version.h"

namespace {
void Init(HMODULE self) {
    melange::game::Init(self);
    melange::log::Init(melange::game::DataDir() + L"\\Melange.log");
    melange::config::Init(melange::game::PluginDir() + L"\\Melange.ini");

    const auto& exe = melange::game::Exe();
    LOG_INFO("Melange " MELANGE_VERSION " starting (pid %lu)", GetCurrentProcessId());
    LOG_INFO("game dir : %s", melange::game::Narrow(melange::game::GameDir()).c_str());
    LOG_INFO("ini      : %s", melange::game::Narrow(melange::config::Path()).c_str());
    LOG_INFO("exe      : size=%u timestamp=%08x sha256=%s", exe.fileSize, exe.timestamp, exe.sha256.c_str());
    if (exe.known)
        LOG_INFO("exe guard: recognised build %s - all modules available", exe.build);
    else
        LOG_WARN("exe guard: UNRECOGNISED build - modules using fixed addresses are disabled");

    melange::events::InstallCore();
    melange::modules::InstallAll();
    LOG_INFO("startup complete, %zu modules active", melange::modules::Installed().size());
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Init(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        melange::modules::UninstallAll();
    }
    return TRUE;
}
