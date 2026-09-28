// Melange entry point. Loaded by Ultimate ASI Loader (dinput8.dll) as melange.asi.
#include <windows.h>

#include "core/compat.h"
#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "version.h"

namespace {
// Ultimate ASI Loader loads .asi files in directory order, so at DllMain time an old WUMFix.asi that
// sorts after melange.asi isn't loaded yet and the immediate check below misses it. Recheck shortly
// after, once the loader has finished, and back out if it turns out we were double-loaded.
DWORD WINAPI RecheckDoubleLoad(LPVOID) {
    Sleep(250);
    if (melange::compat::OldWumfixAlsoLoaded()) {
        WF_ERROR("old WUMFix.asi also installed - delete it");
        melange::modules::UninstallAll();
    }
    return 0;
}

void Init(HMODULE self) {
    melange::game::Init(self);
    melange::log::Init(melange::game::DataDir() + L"\\Melange.log");
    melange::compat::MigrateFromWumfix();
    melange::config::Init(melange::game::PluginDir() + L"\\Melange.ini");

    const auto& exe = melange::game::Exe();
    WF_INFO("Melange " MELANGE_VERSION " starting (pid %lu)", GetCurrentProcessId());
    WF_INFO("game dir : %s", melange::game::Narrow(melange::game::GameDir()).c_str());
    WF_INFO("ini      : %s", melange::game::Narrow(melange::config::Path()).c_str());
    WF_INFO("exe      : size=%u timestamp=%08x sha256=%s", exe.fileSize, exe.timestamp, exe.sha256.c_str());
    if (exe.known)
        WF_INFO("exe guard: recognised build %s - all modules available", exe.build);
    else
        WF_WARN("exe guard: UNRECOGNISED build - modules using fixed addresses are disabled");

    if (melange::compat::OldWumfixAlsoLoaded()) {
        WF_ERROR("old WUMFix.asi also installed - delete it");
        return;
    }

    melange::events::InstallCore();
    melange::modules::InstallAll();
    WF_INFO("startup complete, %zu modules active", melange::modules::Installed().size());
    CreateThread(nullptr, 0, &RecheckDoubleLoad, nullptr, 0, nullptr);
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
