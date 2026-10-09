// Handshake: content identity, lobby member data and the host's sim switch.
#include "core/module.h"
#include "melange/mods.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "lua/engine50.h"
#include "lua/sim/bridge_internal.h"
#include "melange/jlog.h"
#include "melange/weapons.h"
#include "mods/handshake_gid.h"
#include "mods/handshake_internal.h"
#include "mods/lobby.h"
#include "mods/thumper_internal.h"
#include "mods/weapon_gate.h"
#include "net/steam.h"
#include "tools/hash.h"
#include "version.h"
#include "weapons/behaviour.h"
#include "weapons/manifest.h"

namespace fs = std::filesystem;

namespace melange::handshake {
namespace {

using SteamID = uint64_t;

// Calls the ISteamMatchmaking008 / ISteamUser016 / ISteamFriends009 vtables directly; installs no hook.
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
using DeleteData_t = bool(__thiscall*)(void*, SteamID, const char*);
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
bool DeleteLobbyDataRaw(SteamID lobby, const char* key) {
    void* obj = Accessor("SteamMatchmaking");
    void** vt = VTable(obj);
    return vt && reinterpret_cast<DeleteData_t>(vt[22])(obj, lobby, key);
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
std::vector<ModMessage> g_hashedMessages;  // what g_content hashed, guarded by g_mx
std::string g_wpnValue, g_msgValue;        // guarded by g_mx
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

// The default contentHash.include convention: spice.json, entry.sim's actual file (wherever it is, not just
// under sim/, since a manifest may point it anywhere inside the mod folder) and <assetsRoot>/**, the mod's own
// assets folder, whatever the mod renames it to with "assets":{"root":...} (weapon banks under it change sim
// data and must be hashed too). This convention stands in for the manifest's real contentHash.include glob list;
// entry.sim is covered explicitly here so a mod whose entry.sim sits outside sim/ still changes the hash when its
// code does.
std::vector<WalkedFile> WalkModFileList(const fs::path& dir, const std::string& entrySimRel, const std::string& assetsRoot,
                                        uint64_t* aggSig) {
    std::vector<WalkedFile> list;
    uint64_t agg = 0;
    std::error_code ec;
    std::set<std::string> seenRel;
    auto add = [&](const fs::path& file) {
        std::error_code e2;
        if (!fs::is_regular_file(file, e2)) return;
        std::string rel = fs::relative(file, dir, e2).generic_string();
        std::transform(rel.begin(), rel.end(), rel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (rel.size() >= 4 && rel.compare(rel.size() - 4, 4, ".csh") == 0) return;  // shadow caches differ per machine
        if (!seenRel.insert(rel).second) return;  // entry.sim may already sit under sim/: don't hash it twice
        uint64_t sig = StatSig(file);
        agg ^= sig;
        list.push_back({file, rel, sig});
    };
    add(dir / "spice.json");
    if (!entrySimRel.empty()) add(dir / entrySimRel);
    for (const std::string& sub : {assetsRoot, std::string("sim")}) {
        fs::path root = dir / sub;
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            add(it->path());
    }
    if (aggSig) *aggSig = agg;
    return list;
}

// Cached by a size/mtime signature per mod. Never runs on the main thread.
std::vector<ContentFile> FilesForMod(const std::string& id, const std::wstring& dirW, const std::string& entrySimRel,
                                     const std::string& assetsRoot) {
    fs::path dir(dirW);
    uint64_t agg = 0;
    std::vector<WalkedFile> list = WalkModFileList(dir, entrySimRel, assetsRoot, &agg);
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

// The content set: mods active this session that are content, have entry.sim or messages, or declare unsafe, in
// load order. Thumper freezes it at launch.
std::vector<thumper::Entry> EnabledContentMods() {
    std::vector<thumper::Entry> out;
    for (thumper::Entry& e : thumper::Snapshot())
        if (e.sessionActive && e.contentRelevant) out.push_back(std::move(e));
    return out;
}

// Taken on the thread that asks for a recompute (the main thread): the message list and the clone table are
// written there, and the background compute only reads these copies.
struct ComputeInputs {
    std::vector<ModMessage> messages;
    std::vector<weapons::manifest::CloneDecl> clones;
    int extraPerExplosion = 8;
};

ComputeInputs TakeInputs() {
    ComputeInputs in;
    for (auto& [name, id] : simbridge::ModMessages()) in.messages.push_back({name, id});
    if (wpngate::LocalClones()) {
        in.clones = weapons::manifest::Frozen();
        // A per-machine [Weapons] setting, but one that changes clone sim behaviour (how many extra explosions a
        // clone's Lua handler may queue), so it goes into the content text alongside the clones themselves.
        in.extraPerExplosion = weapons::behaviour::ExtraLimit();
    }
    return in;
}

std::string SetValueText(const weapons::manifest::SetValue& v) {
    using weapons::FieldType;
    switch (v.type) {
        case FieldType::Bool: return SetBool(v.boolean);
        case FieldType::String: return SetString(v.string);
        default: return SetNumber(v.number);
    }
}

std::string LowerSlashes(std::string s) {
    for (char& c : s) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<CloneSpec> CloneSpecs(const std::vector<weapons::manifest::CloneDecl>& decls,
                                  const std::vector<ContentMod>& mods,
                                  const std::vector<thumper::Entry>& enabled) {
    std::vector<CloneSpec> out;
    for (const auto& d : decls) {
        CloneSpec c;
        c.k = d.k;
        c.vid = weapons::kVidBase + d.k;
        c.name = d.name;
        c.base = d.base;
        c.mod = d.mod;
        c.cell = d.cell;
        if (!d.bank.empty()) {
            c.bankSha256 = "missing";
            std::string root = "assets";
            for (const auto& e : enabled)
                if (e.manifest.id == d.mod) root = e.manifest.assetsRoot;
            const std::string rel = LowerSlashes(root) + "/data/" + LowerSlashes(d.bank);
            for (const auto& m : mods)
                if (m.id == d.mod)
                    for (const auto& f : m.files)
                        if (f.relPath == rel) c.bankSha256 = f.sha256Hex;
        }
        for (const auto& v : d.set) c.set.emplace_back(v.field, SetValueText(v));
        out.push_back(std::move(c));
    }
    return out;
}

struct Computed {
    mods::ContentId id{};
    std::vector<ModMessage> messages;
    std::string wpn, msg;
};

Computed ComputeContent(const ComputeInputs& in) {
    std::vector<thumper::Entry> enabled = EnabledContentMods();
    std::vector<ContentMod> contentMods;
    contentMods.reserve(enabled.size());
    for (const thumper::Entry& m : enabled)
        contentMods.push_back(
            {m.manifest.id, m.manifest.version, FilesForMod(m.manifest.id, m.dir, m.manifest.entrySim, m.manifest.assetsRoot)});
    const std::vector<CloneSpec> clones = CloneSpecs(in.clones, contentMods, enabled);
    Computed out;
    out.id = BuildContentId(std::move(contentMods), in.messages, clones, in.extraPerExplosion);
    out.messages = in.messages;
    out.wpn = BuildWpnValue(clones, in.extraPerExplosion);
    out.msg = BuildMsgValue(in.messages);
    return out;
}

void PublishOwnMemberData() {
    SteamID lobby = g_lobby.load();
    if (!lobby || !g_publish) return;
    mods::ContentId c;
    std::string wpn, msg;
    {
        std::lock_guard lk(g_mx);
        c = g_content;
        wpn = g_wpnValue;
        msg = g_msgValue;
    }
    SetLobbyMemberDataRaw(lobby, "mlg", BuildMlgValue(MELANGE_VERSION, c).c_str());
    std::vector<ContentMod> ids;
    for (const thumper::Entry& m : EnabledContentMods()) ids.push_back({m.manifest.id, m.manifest.version, {}});
    if (!ids.empty()) SetLobbyMemberDataRaw(lobby, "mlg.mods", BuildModsValue(ids).c_str());
    if (!msg.empty()) SetLobbyMemberDataRaw(lobby, "mlg.msg", msg.c_str());
    if (!wpn.empty()) SetLobbyMemberDataRaw(lobby, "mlg.wpn", wpn.c_str());
    jlog::Rec("handshake", jlog::Level::Info, "publish").Str("hash16", Hash16(c)).Uint("contentMods", c.contentMods)
        .Uint("messages", c.modMessages).Str("wpn", wpn).Emit();
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
    if (!sim.empty() && LobbyData(lobby, "mlg.sim") != sim) SetLobbyDataRaw(lobby, "mlg.sim", sim.c_str());
    std::string wpn;
    { std::lock_guard lk(g_mx); wpn = g_wpnValue; }
    const std::string req = BuildReqValue(ourHash16, !wpn.empty());
    const std::string have = LobbyData(lobby, "mlg.req");
    if (!req.empty() && have != req) SetLobbyDataRaw(lobby, "mlg.req", req.c_str());
    else if (req.empty() && !have.empty()) DeleteLobbyDataRaw(lobby, "mlg.req");
}

std::atomic<uint32_t> g_computeGen{0};

void Recompute(uint32_t gen, const ComputeInputs& in) {
    Computed c = ComputeContent(in);
    {
        std::lock_guard lk(g_mx);
        if (gen != g_computeGen.load()) return;  // a newer computation superseded this one
        g_content = c.id;
        g_hashedMessages = std::move(c.messages);
        g_wpnValue = std::move(c.wpn);
        g_msgValue = std::move(c.msg);
    }
    PublishOwnMemberData();
    RewriteMlgSimIfOwner();
}
void RecomputeAsync() {
    const uint32_t gen = ++g_computeGen;
    std::thread([gen, in = TakeInputs()] { Recompute(gen, in); }).detach();
}

// At Init: every hashed mod message must still have the id the content text names.
bool MessageIdsUnchanged() {
    if (!melange::game::IsKnownBuild()) return true;
    std::vector<ModMessage> hashed;
    { std::lock_guard lk(g_mx); hashed = g_hashedMessages; }
    const std::vector<std::string> changed = ChangedMessageIds(hashed, &melange::lua50::Lookup);
    if (changed.empty()) return true;
    std::string names;
    for (const auto& n : changed) names += (names.empty() ? "" : ", ") + n;
    LOG_ERROR("[handshake] message ids changed since the content hash (%s): content mods are off for this match",
              names.c_str());
    jlog::Rec("handshake", jlog::Level::Error, "message_ids_changed").Str("names", names).Emit();
    return false;
}

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
    std::string why;
    const bool clonesOk = mods::CloneLobbyOk(&why);
    const bool idsOk = MessageIdsUnchanged();
    if (allowed && !clonesOk) LOG_WARN("[handshake] content mods are off for this match: %s", why.c_str());
    allowed = allowed && clonesOk && idsOk;
    g_simAllowedThisMatch = allowed;
    if (!allowed && !c.vanilla)
        LOG_WARN("[handshake] content mods are suspended for this online match (lobby content does not match)");
    jlog::Rec("handshake", jlog::Level::Info, "gate").Bool("allowed", allowed).Bool("online", lobby != 0)
        .Bool("clones", clonesOk).Bool("messageIds", idsOk).Emit();
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

std::string PeerModsDiff(uint64_t steamId) {
    SteamID lobby = g_lobby.load();
    if (!lobby) return "";
    std::vector<ContentMod> ids;
    for (const thumper::Entry& m : EnabledContentMods()) ids.push_back({m.manifest.id, m.manifest.version, {}});
    return DiffModsValues(BuildModsValue(ids), LobbyMemberData(lobby, steamId, "mlg.mods"));
}

std::string PeerMsgDiff(uint64_t steamId) {
    SteamID lobby = g_lobby.load();
    if (!lobby) return "";
    std::string ours;
    { std::lock_guard lk(g_mx); ours = g_msgValue; }
    return DiffMsgValues(ours, LobbyMemberData(lobby, steamId, "mlg.msg"));
}

}  // namespace melange::handshake

namespace melange::mods {
ContentId LocalContent() {
    std::lock_guard lk(handshake::g_mx);
    return handshake::g_content;
}

int Peers(Peer* out, int max) {
    using namespace melange::handshake;
    SteamID lobby = g_lobby.load();
    if (!lobby) return 0;
    SteamID me = MySteamId();
    if (!out || max <= 0) {
        int others = 0;
        for (int i = 0, n = NumLobbyMembers(lobby); i < n; ++i)
            if (LobbyMemberByIndex(lobby, i) != me) ++others;
        return others;
    }
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
bool InLobby() { return handshake::g_lobby.load() != 0; }
}  // namespace melange::mods

namespace melange::handshake::lobby {
uint64_t Current() { return g_lobby.load(); }
uint64_t Me() { return MySteamId(); }
uint64_t Owner() {
    const SteamID l = g_lobby.load();
    return l ? LobbyOwner(l) : 0;
}
std::vector<uint64_t> Members() {
    std::vector<uint64_t> out;
    const SteamID l = g_lobby.load();
    if (!l) return out;
    const SteamID me = MySteamId();
    for (int i = 0, n = NumLobbyMembers(l); i < n; ++i)
        if (SteamID m = LobbyMemberByIndex(l, i); m && m != me) out.push_back(m);
    return out;
}
std::string MemberData(uint64_t member, const char* key) {
    const SteamID l = g_lobby.load();
    return l ? LobbyMemberData(l, member, key) : std::string();
}
void SetMyData(const char* key, const char* value) {
    if (const SteamID l = g_lobby.load()) SetLobbyMemberDataRaw(l, key, value);
}
std::string Name(uint64_t member) {
    const std::string n = FriendName(member);
    return n.empty() ? std::to_string(member) : n;
}
std::string Data(const char* key) {
    const SteamID l = g_lobby.load();
    return l ? LobbyData(l, key) : std::string();
}
}  // namespace melange::handshake::lobby

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
        melange::config::EnsureKey("Handshake", "WeaponGate", "refuse");
        const std::string policy = melange::config::GetString("Handshake", "WeaponGate", "refuse");
        wpngate::Install(policy == "suspend" ? wpngate::Policy::Suspend : wpngate::Policy::Refuse,
                         Bool("LeaveButton", true));
        // Game-file integrity: its own member key, never part of the content hash (it only warns).
        gid::Install(Bool("PeerIntegrity", true), g_publish);
        new Listener(melange::steam::kLobbyEnter, 24);
        new Listener(melange::steam::kLobbyDataUpdate, 24);
        new Listener(melange::steam::kLobbyChatUpdate, 32);
        new Listener(melange::steam::kLobbyKicked, 24);
        melange::mods::OnChange(&OnModsChanged, nullptr);
        melange::simbridge::SetGate(&HandshakeGate);
        // Steam's own LeaveLobby has no callback of its own (unlike a kick), so the only other signal that we
        // left is the engine's net session closing: NetSession fires this generically, independent of Handshake.
        melange::events::Subscribe(melange::events::Event::LobbyLeave, [] {
            g_lobby = 0;
            g_simAllowedThisMatch = false;
            melange::jlog::Rec("handshake", melange::jlog::Level::Info, "left_lobby").Emit();
        });
        RegisterPanel();
        RecomputeAsync();
        return true;
    }
};
}  // namespace

MELANGE_MODULE(Handshake);
