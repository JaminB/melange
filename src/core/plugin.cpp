// WUMFix entry point. Loaded by Ultimate ASI Loader (dinput8.dll) as WUMFix.asi.
#include <windows.h>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "version.h"

namespace {
void Init(HMODULE self) {
    wf::game::Init(self);
    wf::log::Init(wf::game::DataDir() + L"\\WUMFix.log");
    wf::config::Init(wf::game::PluginDir() + L"\\WUMFix.ini");

    const auto& exe = wf::game::Exe();
    WF_INFO("WUMFix " WUMFIX_VERSION " starting (pid %lu)", GetCurrentProcessId());
    WF_INFO("game dir : %s", wf::game::Narrow(wf::game::GameDir()).c_str());
    WF_INFO("ini      : %s", wf::game::Narrow(wf::config::Path()).c_str());
    WF_INFO("exe      : size=%u timestamp=%08x sha256=%s", exe.fileSize, exe.timestamp, exe.sha256.c_str());
    if (exe.known)
        WF_INFO("exe guard: recognised build %s - all modules available", exe.build);
    else
        WF_WARN("exe guard: UNRECOGNISED build - modules using fixed addresses are disabled");

    wf::events::InstallCore();
    wf::modules::InstallAll();
    WF_INFO("startup complete, %zu modules active", wf::modules::Installed().size());
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Init(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        wf::modules::UninstallAll();
    }
    return TRUE;
}
