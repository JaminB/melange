// Handshake: content identity, lobby member data and the host's sim switch (m2-design.md §3.E).
#include "core/module.h"
#include "melange/mods.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/game.h"
#include "core/log.h"
#include "lua/engine50.h"
#include "lua/sim/bridge_internal.h"
#include "melange/jlog.h"
#include "mods/handshake_internal.h"
#include "net/steam.h"
#include "tools/hash.h"
#include "version.h"

namespace fs = std::filesystem;

namespace melange::handshake {
namespace {

using SteamID = uint64_t;

// Calls the real ISteamMatchmaking008 / ISteamUser016 / ISteamFriends009 vtables at the slots re-verified in
// melange-private/re/ISteamMatchmaking008.h. This never installs a hook: LocalNet (private, test-only) patches the
// interface OBJECT's vtable in place (see net/steam_trace.cpp's HookMatchmaking), so a plain call through the
// normal accessor sees LocalNet's answers during tests and the real Steam client otherwise.
void* Accessor(const char* name) {
    HMODULE api = GetModuleHandleW(L"steam_api.dll");
    if (!api) return nullptr;
    using Fn = void*(__cdecl*)();
    auto fn = reinterpret_cast<Fn>(GetProcAddress(api, name));
    return fn ? fn() : nullptr;
}
void** VTable(void* obj) { return obj ? *static_cast<void***>(obj) : nullptr; }

using GetNumMembers_t = int(__thiscall*)(void*, SteamID);
using MemberByIndex_t = SteamID*(__thiscall*)(void*, SteamID*, SteamID, int);
using GetData_t = const char*(__thiscall*)(void*, SteamID, const char*);
using SetData_t = bool(__thiscall*)(void*, SteamID, const char*, const char*);
using GetMemberData_t = const char*(__thiscall*)(void*, SteamID, SteamID, const char*);
using SetMemberData_t = void(__thiscall*)(void*, SteamID, const char*, const char*);
using GetOwner_t = SteamID*(__thiscall*)(void*, SteamID*, SteamID);
using GetSteamID_t = SteamID*(__thiscall*)(void*, SteamID*);
using GetFriendName_t = const char*(__thiscall*)(void*, SteamID);

int NumLobbyMembers(SteamID lobby) {
    void* obj = Accessor("SteamMatchmaking");
    void** vt = VTable(obj);
    return vt ? reinterpret_cast<GetNumMembers_t>(vt[16])(obj, lobby) : 0;
}
SteamID LobbyMemberByIndex(SteamID lobby, int i) {
    void* obj = Accessor("SteamMatchmaking");
    void** vt = VTable(obj);
    SteamID out = 0;
    if (vt) reinterpret_cast<MemberByIndex_t>(vt[17])(obj, &out, lobby, i);
    return out;
}
std::string LobbyMemberData(SteamID lobby, SteamID user, const char* key) {
    void* obj = Accessor("SteamMatchmaking");
    void** vt = VTable(obj);
    if (!vt) return {};
    const char* v = reinterpret_cast<GetMemberData_t>(vt[23])(obj, lobby, user, key);
    return v ? v : "";
}
void SetLobbyMemberDataRaw(SteamID lobby, const char* key, const char* value) {
    void* obj = Accessor("SteamMatchmaking");
    void** vt = VTable(obj);
    if (vt) reinterpret_cast<SetMemberData_t>(vt[24])(obj, lobby, key, value);
}
bool SetLobbyDataRaw(SteamID lobby, const char* key, const char* value) {
    void* obj = Accessor("SteamMatchmaking");
    void** vt = VTable(obj);
    return vt && reinterpret_cast<SetData_t>(vt[19])(obj, lobby, key, value);
}
std::string LobbyData(SteamID lobby, const char* key) {
    void* obj = Accessor("SteamMatchmaking");
    void** vt = VTable(obj);
    if (!vt) return {};
    const char* v = reinterpret_cast<GetData_t>(vt[18])(obj, lobby, key);
    return v ? v : "";
}
SteamID LobbyOwner(SteamID lobby) {
    void* obj = Accessor("SteamMatchmaking");
    void** vt = VTable(obj);
    SteamID out = 0;
    if (vt) reinterpret_cast<GetOwner_t>(vt[34])(obj, &out, lobby);
    return out;
}
SteamID MySteamId() {
    void* obj = Accessor("SteamUser");
    void** vt = VTable(obj);
    SteamID out = 0;
    if (vt) reinterpret_cast<GetSteamID_t>(vt[2])(obj, &out);
    return out;
}
std::string FriendName(SteamID id) {
    void* obj = Accessor("SteamFriends");
    void** vt = VTable(obj);
    if (!vt) return {};
    const char* n = reinterpret_cast<GetFriendName_t>(vt[7])(obj, id);
    return n ? n : "";
}

// ---------------------------------------------------------------- state
std::mutex g_mx;
mods::ContentId g_content{};  // vanilla-initialised until the first background compute finishes
std::atomic<uint64_t> g_lobby{0};
std::atomic<bool> g_simAllowedThisMatch{false};
bool g_publish = true;
bool g_simOnline = true;

struct WalkedFile { fs::path abs; std::string rel; uint64_t sig; };
struct CachedMod { std::string id; uint64_t sig = 0; std::vector<ContentFile> files; };
std::vector<CachedMod> g_fileCache;  // guarded by g_mx

uint64_t StatSig(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    auto t = fs::last_write_time(p, ec);
    return (static_cast<uint64_t>(sz) * 1099511628211ULL) ^ static_cast<uint64_t>(t.time_since_epoch().count());
}

// Default contentHash.include (m2-design.md §3.E): spice.json, the entry.sim folder, assets/**. ModInfo (frozen)
// does not carry the manifest's actual entrySim path or its hashInclude override, so this walks the conventional
// "sim/" folder in its place; see the "API deviations" note in the task report for the follow-up once Thumper (A)
// can hand E the real per-mod include list.
std::vector<WalkedFile> WalkModFileList(const fs::path& dir, uint64_t* aggSig) {
    std::vector<WalkedFile> list;
    uint64_t agg = 0;
    std::error_code ec;
    auto add = [&](const fs::path& file) {
        std::error_code e2;
        if (!fs::is_regular_file(file, e2)) return;
        uint64_t sig = StatSig(file);
        agg ^= sig;
        std::string rel = fs::relative(file, dir, e2).generic_string();
        std::transform(rel.begin(), rel.end(), rel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        list.push_back({file, rel, sig});
    };
    add(dir / "spice.json");
    for (const char* sub : {"assets", "sim"}) {
        fs::path root = dir / sub;
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            add(it->path());
    }
    if (aggSig) *aggSig = agg;
    return list;
}

// Cached by aggregate size/mtime signature (m2-design.md: "cached by file size and mtime"): file contents are
// only re-hashed for a mod whose signature changed since the last call. Never runs on the main thread.
std::vector<ContentFile> FilesForMod(const std::string& id, const std::wstring& dirW) {
    fs::path dir(dirW);
    uint64_t agg = 0;
    std::vector<WalkedFile> list = WalkModFileList(dir, &agg);
    std::lock_guard lk(g_mx);
    for (auto& c : g_fileCache) {
        if (c.id != id) continue;
        if (c.sig == agg) return c.files;
        std::vector<ContentFile> files;
        files.reserve(list.size());
        for (auto& w : list) files.push_back({w.rel, hashutil::Sha256HexFile(w.abs.wstring())});
        c.sig = agg;
        c.files = files;
        return files;
    }
    std::vector<ContentFile> files;
    files.reserve(list.size());
    for (auto& w : list) files.push_back({w.rel, hashutil::Sha256HexFile(w.abs.wstring())});
    g_fileCache.push_back({id, agg, files});
    return files;
}

// Enabled mods that count as content (m2-design.md §3.E): Thumper is expected to have already folded "has
// entry.sim", "declares messages" and "unsafe" into Kind::Content when it resolves the manifest (§2.5), so a
// plain Kind check here is enough and needs no extra per-mod flags from the frozen ModInfo.
std::vector<mods::ModInfo> EnabledContentMods() {
    std::vector<mods::ModInfo> all(256);
    int n = mods::List(all.data(), static_cast<int>(all.size()));
    n = std::min(n, static_cast<int>(all.size()));
    std::vector<mods::ModInfo> out;
    for (int i = 0; i < n; ++i) {
        const mods::ModInfo& m = all[static_cast<size_t>(i)];
        if (m.state == mods::State::Enabled && m.kind == mods::Kind::Content) out.push_back(m);
    }
    return out;
}

// Deviation (documented): the spec's canonical text folds "every registered mod message as name=id in
// registration order". No frozen contract yet hands a non-registering component that ordered (name, id) list —
// only Thumper (A) knows it, at the moment it calls sim::RegisterModMessage. Until then this folds the overhang
// past the known vanilla floor as a count, which still changes the hash whenever the message set does.
uint32_t RegisteredModMessages() {
    if (!melange::game::IsKnownBuild()) return 0;
    uint32_t count = melange::lua50::RegistryCount();
    return count > kVanillaMessageCount ? count - kVanillaMessageCount : 0;
}

mods::ContentId ComputeContent() {
    std::vector<mods::ModInfo> enabled = EnabledContentMods();
    std::vector<ContentMod> contentMods;
    contentMods.reserve(enabled.size());
    for (const mods::ModInfo& m : enabled) {
        std::string id = m.id ? m.id : "";
        contentMods.push_back({id, m.version ? m.version : "", FilesForMod(id, m.dir ? m.dir : L"")});
    }
    return BuildContentId(std::move(contentMods), RegisteredModMessages());
}

void PublishOwnMemberData() {
    SteamID lobby = g_lobby.load();
    if (!lobby || !g_publish) return;
    mods::ContentId c;
    { std::lock_guard lk(g_mx); c = g_content; }
    SetLobbyMemberDataRaw(lobby, "mlg", BuildMlgValue(MELANGE_VERSION, c).c_str());
    std::vector<ContentMod> ids;
    for (const mods::ModInfo& m : EnabledContentMods()) ids.push_back({m.id ? m.id : "", m.version ? m.version : "", {}});
    SetLobbyMemberDataRaw(lobby, "mlg.mods", BuildModsValue(ids).c_str());
    jlog::Rec("handshake", jlog::Level::Info, "publish").Str("hash16", Hash16(c)).Uint("contentMods", c.contentMods).Emit();
}

void RewriteMlgSimIfOwner() {
    SteamID lobby = g_lobby.load();
    if (!lobby || LobbyOwner(lobby) != MySteamId()) return;
    mods::ContentId c;
    { std::lock_guard lk(g_mx); c = g_content; }
    std::string ourHash16 = Hash16(c);
    SteamID me = MySteamId();
    std::vector<std::string> memberHashes;
    int n = NumLobbyMembers(lobby);
    for (int i = 0; i < n; ++i) {
        SteamID member = LobbyMemberByIndex(lobby, i);
        if (member == me) continue;
        std::string version, hash16;
        uint32_t cnt = 0;
        if (ParseMlgValue(LobbyMemberData(lobby, member, "mlg"), &version, &hash16, &cnt))
            memberHashes.push_back(hash16);
        else
            memberHashes.push_back("v");  // no (or malformed) mlg key: never matches, as a vanilla member should
    }
    std::string sim = BuildMlgSim(ourHash16, c.vanilla, memberHashes);
    if (!sim.empty()) SetLobbyDataRaw(lobby, "mlg.sim", sim.c_str());
}

void Recompute() {
    mods::ContentId c = ComputeContent();
    { std::lock_guard lk(g_mx); g_content = c; }
    PublishOwnMemberData();
    RewriteMlgSimIfOwner();
}
void RecomputeAsync() { std::thread([] { Recompute(); }).detach(); }

bool HandshakeGate() {
    mods::ContentId c;
    { std::lock_guard lk(g_mx); c = g_content; }
    SteamID lobby = g_lobby.load();
    bool allowed;
    if (!lobby)
        allowed = true;  // offline / local: always allowed (mods.h: "true offline when content mods exist")
    else if (!g_simOnline)
        allowed = false;
    else
        allowed = GateAllowsSim(true, Hash16(c), LobbyData(lobby, "mlg.sim"));
    g_simAllowedThisMatch = allowed;
    if (!allowed && !c.vanilla)
        LOG_WARN("[handshake] content mods are suspended for this online match (lobby content does not match)");
    jlog::Rec("handshake", jlog::Level::Info, "gate").Bool("allowed", allowed).Bool("online", lobby != 0).Emit();
    return allowed;
}

void OnModsChanged(void*) { RecomputeAsync(); }

#pragma pack(push, 8)
struct LobbyEnterCb { SteamID lobby; uint32_t chatPermissions; bool locked; uint32_t response; };
struct LobbyDataUpdateCb { SteamID lobby; SteamID member; uint8_t success; };
struct LobbyChatUpdateCb { SteamID lobby; SteamID userChanged; SteamID makingChange; uint32_t stateChange; };
#pragma pack(pop)
static_assert(sizeof(LobbyEnterCb) == 24);
static_assert(sizeof(LobbyDataUpdateCb) == 24);
static_assert(sizeof(LobbyChatUpdateCb) == 32);

// Same shape as net/steam_trace.cpp's Listener: one CallbackBase per callback id, kept alive for the process.
class Listener final : public melange::steam::CallbackBase {
public:
    Listener(int id, int size) : size_(size) { melange::steam::RegisterCallback(this, id); }
    void Run(void* p) override { Log(p); }
    void Run(void* p, bool, melange::steam::SteamAPICall) override { Log(p); }
    int GetCallbackSizeBytes() override { return size_; }

private:
    int size_;
    void Log(void* p) {
        switch (callbackId_) {
        case melange::steam::kLobbyEnter:
            g_lobby = static_cast<LobbyEnterCb*>(p)->lobby;
            RecomputeAsync();
            break;
        case melange::steam::kLobbyChatUpdate:
            if (static_cast<LobbyChatUpdateCb*>(p)->lobby == g_lobby.load()) RewriteMlgSimIfOwner();
            break;
        case melange::steam::kLobbyDataUpdate: {
            auto* e = static_cast<LobbyDataUpdateCb*>(p);
            if (e->lobby == g_lobby.load() && e->member != MySteamId()) RewriteMlgSimIfOwner();
            break;
        }
        case melange::steam::kLobbyKicked:
            g_lobby = 0;
            g_simAllowedThisMatch = false;
            break;
        default:
            break;
        }
    }
};

}  // namespace

void RegisterPanel();  // handshake_panel.cpp

}  // namespace melange::handshake

namespace melange::mods {
ContentId LocalContent() {
    std::lock_guard lk(handshake::g_mx);
    return handshake::g_content;
}

int Peers(Peer* out, int max) {
    using namespace melange::handshake;
    SteamID lobby = g_lobby.load();
    if (!lobby || max <= 0) return 0;
    SteamID me = MySteamId();
    ContentId c;
    { std::lock_guard lk(g_mx); c = g_content; }
    std::string ourHash16 = Hash16(c);
    int written = 0;
    int n = NumLobbyMembers(lobby);
    for (int i = 0; i < n && written < max; ++i) {
        SteamID member = LobbyMemberByIndex(lobby, i);
        if (member == me) continue;
        Peer p{};
        p.steamId = member;
        std::string name = FriendName(member);
        snprintf(p.name, sizeof(p.name), "%s", name.empty() ? std::to_string(member).c_str() : name.c_str());
        std::string version, hash16;
        uint32_t cnt = 0;
        bool has = ParseMlgValue(LobbyMemberData(lobby, member, "mlg"), &version, &hash16, &cnt);
        p.status = ClassifyPeer(has, has ? hash16 : "", ourHash16);
        snprintf(p.hash16, sizeof(p.hash16), "%s", (has ? hash16 : "").c_str());
        snprintf(p.version, sizeof(p.version), "%s", (has ? version : "").c_str());
        out[written++] = p;
    }
    return written;
}

bool SimAllowedThisMatch() { return handshake::g_simAllowedThisMatch.load(); }
}  // namespace melange::mods

namespace {
class Handshake final : public melange::Module {
public:
    const char* Name() const override { return "Handshake"; }
    const char* Description() const override { return "content mod identity and the online lobby handshake"; }
    int Order() const override { return 57; }
    bool Install() override {
        using namespace melange::handshake;
        g_publish = Bool("Publish", true);
        g_simOnline = Bool("SimOnline", true);
        new Listener(melange::steam::kLobbyEnter, 24);
        new Listener(melange::steam::kLobbyDataUpdate, 24);
        new Listener(melange::steam::kLobbyChatUpdate, 32);
        new Listener(melange::steam::kLobbyKicked, 24);
        melange::mods::OnChange(&OnModsChanged, nullptr);
        melange::simbridge::SetGate(&HandshakeGate);
        RegisterPanel();
        RecomputeAsync();
        return true;
    }
};
}  // namespace

MELANGE_MODULE(Handshake);
