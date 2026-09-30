// level.* in the game's Oasis: the Erg level service over the install and the map packs Thumper enabled this launch.
// Every method runs on a server thread and never touches the game.
#include <windows.h>

#include <shlobj.h>

#include <memory>

#include "assets/crcsafe.h"
#include "core/config.h"
#include "core/game.h"
#include "core/log.h"
#include "erg/service.h"
#include "levels/gate.h"
#include "mods/thumper_internal.h"
#include "oasis/core/router.h"
#include "oasis/providers.h"
#include "tools/json_read.h"

namespace melange::oasis::providers {
namespace {
std::unique_ptr<erg::service::Service> g_service;
std::vector<assets::crcsafe::Entry> g_crc;

std::wstring ProjectsDir() {
    const std::string ini = config::GetString("Erg", "ProjectsDir", "");
    if (!ini.empty()) {
        const std::wstring w = game::Widen(ini);
        return w.size() > 1 && w[1] == L':' ? w : game::GameDir() + L"\\" + w;
    }
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = std::wstring(docs) + L"\\Melange\\erg\\projects";
    if (docs) CoTaskMemFree(docs);
    return out.empty() ? game::GameDir() + L"\\Melange\\erg\\projects" : out;
}

std::vector<erg::install::Pack> EnabledPacks() {
    std::vector<erg::install::Pack> packs;
    for (const auto& e : thumper::Snapshot()) {
        if (e.state != mods::State::Enabled || !e.sessionActive || !e.manifest.content) continue;
        erg::install::Pack p;
        if (erg::install::PackFromManifest(e.manifest, e.dir, &p)) packs.push_back(std::move(p));
    }
    return erg::install::AssignPacks(std::move(packs));
}

bool ModActive(const std::string& id) {
    thumper::Entry e;
    return thumper::FindEntry(id, &e) && e.sessionActive && e.state == mods::State::Enabled;
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

bool BuildTestLevel(const std::string& project, const std::string& tod, std::string* stem, std::string* title, std::string* err) {
    if (!g_service) {
        *err = "the level service is off ([Erg] Enabled=0)";
        return false;
    }
    erg::service::Reply rep = g_service->BuildTest(project, game::GameDir() + L"\\Melange\\erg\\test", tod);
    if (!rep.ok) {
        *err = rep.message;
        return false;
    }
    json::Value v;
    json::Error perr;
    const json::Value *s = nullptr, *t = nullptr;
    if (!json::Parse(rep.json, &v, &perr) || !(s = v.Get("stem")) || !(t = v.Get("title")) || !s->IsString() || !t->IsString()) {
        *err = "test build: bad reply";
        return false;
    }
    *stem = s->string;
    *title = t->string;
    return true;
}

void InstallLevels() {
    config::EnsureKey("Erg", "Enabled", "1");
    config::EnsureKey("Erg", "ProjectsDir", "");
    if (!config::GetBool("Erg", "Enabled", true)) {
        LOG_INFO("[erg] level service off ([Erg] Enabled=0)");
        return;
    }
    erg::service::Env env;
    env.gameDir = game::GameDir();
    env.projectsDir = ProjectsDir();
    env.packs = &EnabledPacks;
    env.modsReadOnly = [] { return false; };
    env.modActive = &ModActive;
    if (assets::crcsafe::Available()) g_crc = assets::crcsafe::Entries();
    env.crcCollides = [](const std::string& name) { return assets::crcsafe::Collides(g_crc, name); };
    env.inSession = [](const std::string& id) { return ModActive(id) && levels::gate::InLobby(); };
    env.readOnly = [] { return core::router::ReadOnly(); };
    g_service = std::make_unique<erg::service::Service>(std::move(env));
    for (const auto& m : erg::service::Methods())
        AddMethod(m.c_str(), &Handle, nullptr, kRpcServerThread | (erg::service::Mutating(m) ? kRpcMutating : kRpcNone));
    LOG_INFO("[erg] level service: %zu methods, projects in %s", erg::service::Methods().size(), game::Narrow(ProjectsDir()).c_str());
}
}  // namespace melange::oasis::providers
