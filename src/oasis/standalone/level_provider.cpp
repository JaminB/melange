#include "oasis/standalone/level_provider.h"

#include <windows.h>

#include <shlobj.h>

#include <memory>

#include "erg/service.h"
#include "melange/oasis.h"
#include "oasis/core/router.h"
#include "oasis/standalone/game_lock.h"
#include "oasis/standalone/mods_provider.h"

namespace melange::oasis::standalone::levelprov {
namespace {
std::unique_ptr<erg::service::Service> g_service;
std::wstring g_gameDir;
std::string g_version;

std::wstring DefaultProjects(const std::wstring& gameDir) {
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = std::wstring(docs) + L"\\Melange\\erg\\projects";
    if (docs) CoTaskMemFree(docs);
    return out.empty() ? gameDir + L"\\Melange\\erg\\projects" : out;
}

std::vector<erg::install::Pack> EnabledPacks() {
    std::vector<erg::install::Pack> packs;
    for (const auto& m : modsprov::Enabled(g_gameDir, g_version)) {
        erg::install::Pack p;
        if (m.content && erg::install::PackFromManifest(m, m.dir, &p)) packs.push_back(std::move(p));
    }
    return erg::install::AssignPacks(std::move(packs));
}

void Handle(const Call& c, Result& r, void*) {
    erg::service::Reply rep = g_service->Call(c.method, c.paramsJson);
    r.ok = rep.ok;
    r.code = rep.code;
    r.message = std::move(rep.message);
    r.json = std::move(rep.json);
    for (const auto& b : rep.blobs) core::QueueBinary(b.ref, "erg", b.meta, b.bytes);
}
}  // namespace

void Install(const std::wstring& gameDir, const std::wstring& projectsDir, const std::string& melangeVersion) {
    g_gameDir = gameDir;
    g_version = melangeVersion;
    erg::service::Env env;
    env.gameDir = gameDir;
    env.projectsDir = projectsDir.empty() ? DefaultProjects(gameDir) : projectsDir;
    env.packs = &EnabledPacks;
    env.modsReadOnly = [] { return GameRunning(g_gameDir); };
    env.modActive = [](const std::string&) { return false; };
    g_service = std::make_unique<erg::service::Service>(std::move(env));
    for (const auto& m : erg::service::Methods())
        AddMethod(m.c_str(), &Handle, nullptr, kRpcServerThread | (erg::service::Mutating(m) ? kRpcMutating : kRpcNone));
}
}  // namespace melange::oasis::standalone::levelprov
