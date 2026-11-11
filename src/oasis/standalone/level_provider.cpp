#include "oasis/standalone/level_provider.h"

#include <windows.h>

#include <shlobj.h>

#include <cmath>
#include <cstdio>
#include <memory>

#include "assets/crcsafe.h"
#include "erg/service.h"
#include "melange/oasis.h"
#include "oasis/core/router.h"
#include "oasis/standalone/game_lock.h"
#include "oasis/standalone/lua_check.h"
#include "oasis/standalone/mods_provider.h"
#include "oasis/standalone/water_sim.h"
#include "tools/json_read.h"

namespace melange::oasis::standalone::levelprov {
namespace {
std::unique_ptr<erg::service::Service> g_service;
std::wstring g_gameDir;
std::string g_version;
std::vector<assets::crcsafe::Entry> g_crc;

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
    erg::service::Reply rep = g_service->Call(c.method, c.paramsJson, static_cast<uint64_t>(c.client));
    r.ok = rep.ok;
    r.code = rep.code;
    r.message = std::move(rep.message);
    r.json = std::move(rep.json);
    for (const auto& b : rep.blobs) core::QueueBinary(b.ref, "erg", b.meta, b.bytes);
}

// level.script.water {text, turns?, ticksPerTurn?, turnsPerRound?, start?}: the water the script makes by turn
// (water_sim.h); start is world units.
void Water(const Call& c, Result& r, void*) {
    json::Value p;
    json::Error e;
    if (!json::Parse(c.paramsJson, &p, &e) || !p.IsObject()) {
        r.ok = false, r.code = -32602, r.message = "params must be an object";
        return;
    }
    const json::Value* text = p.Get("text");
    if (!text || !text->IsString()) {
        r.ok = false, r.code = -32602, r.message = "text: the script, a string";
        return;
    }
    WaterSim o;
    struct Int {
        const char* key;
        int* out;
        int lo, hi;
    };
    for (const Int& f : {Int{"turns", &o.turns, 1, 1000}, Int{"ticksPerTurn", &o.ticksPerTurn, 1, 100000}, Int{"turnsPerRound", &o.turnsPerRound, 1, 64}}) {
        const json::Value* v = p.Get(f.key);
        if (!v || v->IsNull()) continue;
        if (!v->IsInteger() || v->number < f.lo || v->number > f.hi) {
            r.ok = false, r.code = -32602, r.message = std::string(f.key) + " must be a whole number from " + std::to_string(f.lo) + " to " + std::to_string(f.hi);
            return;
        }
        *f.out = static_cast<int>(v->number);
    }
    if (const json::Value* v = p.Get("start"); v && !v->IsNull()) {
        if (!v->IsNumber() || !(std::fabs(v->number) <= 10000)) {
            r.ok = false, r.code = -32602, r.message = "start must be a number within -10000 and 10000 (world units)";
            return;
        }
        o.start = static_cast<float>(v->number);
    }
    std::string out, err;
    if (!SimulateWater(text->string, o, &out, &err)) {
        r.ok = false, r.code = -32000, r.message = "the script does not compile: " + err;
        return;
    }
    r.json = std::move(out);
}

void HandleClosed(uint64_t connId, void*) {
    if (g_service) g_service->ClientClosed(connId);
}
}  // namespace

std::wstring GameDir() { return g_service ? g_gameDir : std::wstring(); }

bool Build(const std::string& modId, std::string* replyJson, std::string* err) {
    if (!g_service) return *err = "the level service is off ([Erg] Enabled=0)", false;
    std::string id;
    for (char c : modId) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return *err = "bad mod id", false;
        id.push_back(c);
    }
    erg::service::Reply rep = g_service->Call("level.build", "{\"modId\":\"" + id + "\"}");
    if (!rep.ok) return *err = rep.message.empty() ? "the build failed" : rep.message, false;
    *replyJson = std::move(rep.json);
    return true;
}

void Install(const std::wstring& gameDir, const std::wstring& projectsDir, const std::string& melangeVersion) {
    g_gameDir = gameDir;
    g_version = melangeVersion;
    erg::service::Env env;
    env.gameDir = gameDir;
    env.projectsDir = projectsDir.empty() ? DefaultProjects(gameDir) : projectsDir;
    env.packs = &EnabledPacks;
    env.modsReadOnly = [] { return GameRunning(g_gameDir); };
    env.modActive = [](const std::string&) { return false; };
    if (!assets::crcsafe::ReadFromExe(gameDir + L"\\WormsMayhem.exe", &g_crc)) {
        g_crc.clear();
        fwprintf(stderr, L"oasis: the CRC table could not be read from WormsMayhem.exe; built names are not checked\n");
    }
    env.crcCollides = [](const std::string& name) { return assets::crcsafe::Collides(g_crc, name); };
    env.readOnly = [] { return core::router::ReadOnly(); };
    // The script's syntax, with the stock Lua 5.0 parser (the game's dialect): put_script answers syntaxChecked here too.
    env.compile = &CompileLua50;
    env.melangeVersion = melangeVersion;
    g_service = std::make_unique<erg::service::Service>(std::move(env));

    for (const auto& m : erg::service::Methods())
        AddMethod(m.c_str(), &Handle, nullptr, kRpcServerThread | (erg::service::Mutating(m) ? kRpcMutating : kRpcNone));
    AddMethod("level.script.water", &Water, nullptr, kRpcServerThread);
    OnClientClosed(&HandleClosed, nullptr);
}
}  // namespace melange::oasis::standalone::levelprov
