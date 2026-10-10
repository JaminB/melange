// Game-file integrity: which exe and which retail data files this peer runs ("mlg.gid"), and the lobby warning
// when two Melange peers differ. Third-party mods (MMP's SCRIPTS.XOM and CRC-bypassed exe, Wormpot's
// MENUTWKXNET.XOM, Firsacho's lubs, a Renewation/WUM.Loader Data2 overlay) desync silently otherwise. Warning only.
#include "mods/handshake_gid.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "assets/crcsafe.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/thread_guard.h"
#include "core/mem.h"
#include "melange/jlog.h"
#include "melange/testcmd.h"
#include "mods/handshake_internal.h"
#include "mods/lobby.h"
#include "mods/lobbybanner.h"
#include "tools/hash.h"

namespace fs = std::filesystem;

namespace melange::handshake::gid {
namespace lobbybanner = mods::lobbybanner;
namespace {
// The jnz (75 4D) after the per-file CRC compare in the anti-tamper check: CRC bypasses (MMP's exe, WUM.Loader in
// memory) turn it into jmp short (EB). Read only, never patched.
constexpr uintptr_t kCrcBranch = 0x635618;
constexpr uint8_t kJmpShort = 0xEB;
constexpr int kEvalEvery = 30;
constexpr uint32_t kAmber = 0xff30a0ffu;

bool g_enabled = false, g_publish = false;
std::mutex g_mx;
std::string g_exe16, g_data16;          // guarded by g_mx, "" until the background hash finishes
uint32_t g_diskFlags = 0;               // guarded by g_mx: kGidData2
std::vector<std::string> g_warnings;    // guarded by g_mx
std::atomic<bool> g_hashFailed{false};  // the background hash gave up: OurValue stays ""
uint64_t g_publishedLobby = 0;          // main thread
std::string g_publishedValue;           // main thread
int g_banner = 0;

uint32_t LiveFlags() {
    if (!game::IsKnownBuild()) return 0;  // another exe has another layout; its exe hash already differs
    uint8_t b = 0;
    return mem::SafeRead(kCrcBranch, &b, 1) && b == kJmpShort ? kGidCrcOff : 0;
}

void Compute(std::vector<std::string> paths) {
    if (paths.empty()) {
        // Off the known build the live table is not trusted: read the same table from the exe file instead.
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::vector<assets::crcsafe::Entry> table;
        if (assets::crcsafe::ReadFromExe(exe, &table))
            for (const auto& e : table) paths.push_back(e.path);
    }
    const std::string exeSha = game::Exe().sha256;
    if (exeSha.size() < 16) {
        LOG_WARN("[handshake] game files: the exe could not be hashed, so mlg.gid is not published");
        g_hashFailed = true;
        return;
    }
    // Still publish: an unrecognised exe is what peers most need to see. Its data hash then covers no files, so
    // it also reads as different data.
    if (paths.empty()) LOG_WARN("[handshake] game files: the exe's CRC table could not be read, data covers no files");
    const std::wstring dir = game::GameDir();
    std::vector<std::pair<std::string, std::string>> files;
    int missing = 0;
    for (const auto& p : paths) {
        if (!GidHashesPath(p)) continue;
        std::wstring rel = game::Widen(p);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        std::string sha = hashutil::Sha256HexFile(dir + L"\\" + rel);
        if (sha.empty()) ++missing;
        files.emplace_back(p, std::move(sha));
    }
    // A Data2 overlay (WUM.Loader, Renewation) shadows Data\ file by file: hash every file in it, so two peers with
    // different overlays differ in data16, not only in the presence flag.
    std::error_code ec;
    const fs::path data2Dir = fs::path(dir) / L"Data2";
    const bool data2 = fs::is_directory(data2Dir, ec);
    size_t overlay = 0;
    if (data2) {
        try {  // a name the narrow conversion cannot take throws, and this thread must not
            for (fs::recursive_directory_iterator it(data2Dir, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string rel = "Data2/" + it->path().lexically_relative(data2Dir).generic_string();
                if (!GidHashesPath(rel)) continue;
                files.emplace_back(rel, hashutil::Sha256HexFile(it->path().wstring()));
                ++overlay;
            }
            if (ec) LOG_WARN("[handshake] game files: could not list all of Data2 (%s)", ec.message().c_str());
        } catch (const std::exception& e) {
            LOG_WARN("[handshake] game files: could not list all of Data2 (%s)", e.what());
        }
    }
    const std::string data = HashOfCanonicalText(GidDataText(files));
    {
        std::lock_guard lk(g_mx);
        g_exe16 = exeSha.substr(0, 16);
        g_data16 = data.substr(0, std::min<size_t>(16, data.size()));
        g_diskFlags = data2 ? kGidData2 : 0;
    }
    LOG_INFO("[handshake] game files: exe %s, data %s (%zu files, %d missing%s)", exeSha.substr(0, 16).c_str(),
             data.substr(0, 16).c_str(), files.size(), missing,
             data2 ? (", " + std::to_string(overlay) + " of them in the Data2 overlay").c_str() : "");
    jlog::Rec("handshake", jlog::Level::Info, "game_files").Str("exe16", exeSha.substr(0, 16))
        .Str("data16", data.substr(0, 16)).Uint("files", files.size()).Int("missing", missing).Bool("data2", data2)
        .Uint("overlayFiles", overlay).Emit();
}

void UpdateBanner(const std::vector<std::string>& warnings) {
    if (!g_banner) return;
    if (warnings.empty()) lobbybanner::Set(g_banner, kAmber, "", {});
    else lobbybanner::Set(g_banner, kAmber,
                          "Game files differ between players: this match may desync (mods like MMP or Renewation?)",
                          warnings);
}

void Tick() {
    if (events::FrameCount() % kEvalEvery) return;
    const uint64_t l = lobby::Current();
    std::vector<std::string> warnings;
    if (!l) {
        g_publishedLobby = 0;
        g_publishedValue.clear();
    } else {
        const std::string ours = OurValue();
        if (g_publish && !ours.empty() && (l != g_publishedLobby || ours != g_publishedValue)) {
            lobby::SetMyData("mlg.gid", ours.c_str());
            g_publishedLobby = l;
            g_publishedValue = ours;
        }
        std::vector<GidMember> members;
        for (uint64_t m : lobby::Members()) members.push_back({lobby::Name(m), lobby::MemberData(m, "mlg.gid")});
        warnings = GidWarnings(ours, members);
    }
    bool changed;
    {
        std::lock_guard lk(g_mx);
        changed = warnings != g_warnings;
        g_warnings = warnings;
    }
    if (changed) {
        std::string all;
        for (const auto& w : warnings) all += (all.empty() ? "" : "; ") + w;
        if (!warnings.empty()) LOG_WARN("[handshake] game files differ: %s", all.c_str());
        jlog::Rec("handshake", warnings.empty() ? jlog::Level::Info : jlog::Level::Warn, "game_files_lobby")
            .Uint("mismatched", warnings.size()).Str("why", all).Emit();
        UpdateBanner(warnings);  // the slot starts hidden, so only a change needs to reach it
    }
}

bool VerbState(std::string_view, void*) {
    LOG_INFO("[handshake] gid: enabled=%d publish=%d ours='%s' warnings=%zu", g_enabled, g_publish,
             OurValue().c_str(), Warnings().size());
    for (uint64_t m : lobby::Members())
        LOG_INFO("[handshake]   %s gid='%s'", lobby::Name(m).c_str(), lobby::MemberData(m, "mlg.gid").c_str());
    for (const auto& w : Warnings()) LOG_INFO("[handshake]   warn: %s", w.c_str());
    return true;
}
}  // namespace

void Install(bool enabled, bool publish) {
    g_enabled = enabled;
    g_publish = enabled && publish;
    if (!enabled) return;
    // The live table is read here, on the installing thread (crcsafe's lazy load is not thread-safe); the
    // background thread only hashes files.
    std::vector<std::string> paths;
    if (assets::crcsafe::Available())
        for (const auto& e : assets::crcsafe::Entries()) paths.push_back(e.path);
    std::thread([paths = std::move(paths)]() mutable {
        bool done = false;
        GuardedThreadBody("handshake-gid", [&] {
            Compute(std::move(paths));
            done = true;
        });
        if (!done) g_hashFailed = true;  // OurValue stays "" rather than waiting on a hash that is not coming
    }).detach();
    events::Subscribe(events::Event::Frame, [] { Tick(); });
    g_banner = lobbybanner::Add("integrity", 30);
    testcmd::Register("handshake.gid", &VerbState);
}

bool Enabled() { return g_enabled; }
bool HashFailed() { return g_hashFailed.load(); }

std::string OurValue() {
    GameId g;
    {
        std::lock_guard lk(g_mx);
        g = GameId{g_exe16, g_data16, g_diskFlags};
    }
    g.flags |= LiveFlags();
    return BuildGidValue(g);
}

std::vector<std::string> Warnings() {
    std::lock_guard lk(g_mx);
    return g_warnings;
}
}  // namespace melange::handshake::gid
