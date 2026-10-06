// Music: replaces the sudden-death music with MP3s from enabled mods' spice.json "music". At the frontend the tracks
// are scanned (bank.cpp), put in a fresh random order and written as an FSB4 bank under Melange\cache\music. FMOD
// opens Data\Audio\PC\muSuddenDeath.fsb itself with CreateFileA once per match at match load; a hook on that call
// hands it the cache bank instead. The game's own file is never modified. The bank is rebuilt after every match, so
// each match plays its own order.
#include <safetyhook.hpp>
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "levels/engine.h"
#include "melange/jlog.h"
#include "melange/mods.h"
#include "mods/thumper_internal.h"
#include "music/bank.h"
#include "weapons/engine.h"

namespace {
namespace fs = std::filesystem;
namespace mu = melange::music;
namespace weng = melange::weapons::engine;
namespace leng = melange::levels::engine;

constexpr int kSettleFrames = 30;
constexpr int kRetryFrames = 600;     // the bank was busy (the game still had it open): try again
constexpr int kMaxRetries = 3;
constexpr const char* kSlot = "suddenDeath";
constexpr const char* kRel = "Melange/cache/music/suddenDeath.fsb";

int g_frontendFrames = 0;
bool g_wasFront = false;
bool g_dirty = true;                  // startup, the enabled mods changed, or a match ended
int g_retryIn = 0, g_retries = 0;

fs::path GameDir() { return fs::path(melange::game::GameDir()); }
fs::path CacheDir() { return GameDir() / L"Melange" / L"cache" / L"music"; }
fs::path BankPath() { return CacheDir() / L"suddenDeath.fsb"; }

// ---------------------------------------------------------------- the redirect
// The cache bank the hook hands out; empty means pass everything through. Set only after a build verified it.
std::mutex g_mu;
std::wstring g_redirect;
SafetyHookInline g_hook;
bool g_hookOk = false;

std::wstring Redirect() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_redirect;
}
void SetRedirect(const std::wstring& p) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_redirect = p;
}

HANDLE WINAPI HkCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                            HANDLE tmpl) {
    // Read-only opens of the one bank, nothing else.
    if (g_hookOk && disposition == OPEN_EXISTING && !(access & (GENERIC_WRITE | FILE_WRITE_DATA | DELETE)) && mu::IsSuddenDeathPath(name)) {
        const std::wstring path = Redirect();
        // The wide call: the cache path is a wide string (the game folder may not be ANSI), and it does not come back through this hook.
        if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            LOG_INFO("[music] %s: redirected to %s", kSlot, melange::game::Narrow(path).c_str());
            melange::jlog::Rec("music", melange::jlog::Level::Info, "redirect").Str("slot", kSlot);
            return CreateFileW(path.c_str(), access, share, sa, disposition, flags, tmpl);
        }
    }
    return g_hook.stdcall<HANDLE>(name, access, share, sa, disposition, flags, tmpl);
}

// ---------------------------------------------------------------- tracks
struct Cached {
    uintmax_t size = 0;
    fs::file_time_type mtime{};
    mu::Scan scan;
};
std::map<fs::path, Cached> g_scans;                 // parsed once, keyed by path + size + mtime
std::set<std::string> g_loggedSkips;                // so a skipped track is not logged again after every match

struct Item {
    std::string mod, key, title, credit;
    const mu::Track* track = nullptr;
};

const mu::Scan* ScanFile(const fs::path& p, std::string* why) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(p, ec);
    if (ec) {
        *why = "the file is missing";
        return nullptr;
    }
    if (size > mu::kMaxFileBytes) {
        *why = "the file is larger than 24 MiB";
        return nullptr;
    }
    const auto mtime = fs::last_write_time(p, ec);
    auto it = g_scans.find(p);
    if (it != g_scans.end() && it->second.size == size && it->second.mtime == mtime) return &it->second.scan;
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() != size) {
        *why = "the file could not be read";
        return nullptr;
    }
    Cached c;
    c.size = size;
    c.mtime = mtime;
    c.scan = mu::ScanMp3(bytes.data(), bytes.size());
    if (!c.scan.ok) {
        // Reported once per file version; the entry stays cached as refused.
        LOG_ERROR("[music] %s: %s", melange::game::Narrow(p.wstring()).c_str(), c.scan.error.c_str());
        melange::jlog::Rec("music", melange::jlog::Level::Error, "track_refused").Str("file", melange::game::Narrow(p.wstring())).Str("why", c.scan.error);
    }
    return &(g_scans[p] = std::move(c)).scan;
}

bool WriteAtomic(const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code cleanup;
        fs::remove(tmp, cleanup);
        return false;
    }
    return true;
}

// The first 48 bytes of the game's own bank, read once. Empty vector when it is missing or not what we expect.
std::vector<uint8_t> g_vanilla;
bool g_vanillaTried = false;
const std::vector<uint8_t>* Vanilla() {
    if (!g_vanillaTried) {
        g_vanillaTried = true;
        const fs::path p = GameDir() / L"Data" / L"Audio" / L"PC" / L"muSuddenDeath.fsb";
        std::ifstream f(p, std::ios::binary);
        std::vector<uint8_t> b(mu::kHeaderBytes);
        f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
        std::string err;
        if (!f || !mu::CheckVanillaHeader(b.data(), b.size(), &err)) {
            LOG_ERROR("[music] %s: %s; the sudden-death music is left as it is", melange::game::Narrow(p.wstring()).c_str(),
                      f ? err.c_str() : "cannot be read");
            melange::jlog::Rec("music", melange::jlog::Level::Error, "vanilla_unreadable");
        } else {
            g_vanilla = std::move(b);
        }
    }
    return g_vanilla.empty() ? nullptr : &g_vanilla;
}

std::string Duration(uint64_t samples, uint32_t rate) {
    const uint64_t s = rate ? samples / rate : 0;
    char buf[32];
    snprintf(buf, sizeof buf, "%llu:%02llu", static_cast<unsigned long long>(s / 60), static_cast<unsigned long long>(s % 60));
    return buf;
}

void RemoveBank() {
    SetRedirect({});
    std::error_code ec;
    if (fs::remove(BankPath(), ec)) LOG_INFO("[music] %s: no tracks from enabled mods; the game's own music plays", kSlot);
}

// Builds this match's bank from the enabled mods' tracks and points the redirect at it.
void Build() {
    g_dirty = false;
    std::vector<Item> items;
    for (const auto& e : melange::thumper::Snapshot()) {
        if (e.state != melange::mods::State::Enabled) continue;
        for (const auto& m : e.manifest.music) {
            if (m.slot != kSlot) continue;
            const std::string key = e.manifest.id + "/" + m.file;
            if (items.size() >= mu::kMaxPerSlot) {
                if (g_loggedSkips.insert(key).second)
                    LOG_ERROR("[music] %s: more than %zu tracks for %s; skipped", key.c_str(), mu::kMaxPerSlot, kSlot);
                continue;
            }
            std::string why;
            const mu::Scan* s = ScanFile(fs::path(e.dir) / fs::path(std::u8string(m.file.begin(), m.file.end())), &why);
            if (!s && g_loggedSkips.insert(key + "|" + why).second) LOG_ERROR("[music] %s: %s", key.c_str(), why.c_str());
            if (!s || !s->ok) continue;
            items.push_back({e.manifest.id, key, m.title, m.credit, &s->track});
        }
    }
    if (items.empty()) {
        RemoveBank();
        return;
    }
    // The first accepted track fixes the rate and channel count; a track that differs is skipped (no resampling).
    const mu::Track* first = items[0].track;
    std::vector<Item> use;
    for (const auto& it : items) {
        if (it.track->sampleRate != first->sampleRate || it.track->channels != first->channels) {
            if (g_loggedSkips.insert(it.key + "|fmt").second) {
                LOG_ERROR("[music] %s: %u Hz %s differs from %s's %u Hz %s; skipped (all tracks of a slot need the same rate and channels)",
                          it.key.c_str(), it.track->sampleRate, it.track->channels == 1 ? "mono" : "stereo", items[0].key.c_str(),
                          first->sampleRate, first->channels == 1 ? "mono" : "stereo");
                melange::jlog::Rec("music", melange::jlog::Level::Error, "track_skipped").Str("track", it.key);
            }
            continue;
        }
        use.push_back(it);
    }
    const std::vector<uint8_t>* vanilla = Vanilla();
    if (!vanilla) {
        SetRedirect({});
        return;
    }

    // The key of the track that led last time, so two or more tracks never open with the same one twice in a row.
    const fs::path lastPath = CacheDir() / L"last.txt";
    std::string lastKey;
    {
        std::ifstream f(lastPath);
        std::getline(f, lastKey);
        while (!lastKey.empty() && (lastKey.back() == '\r' || lastKey.back() == ' ')) lastKey.pop_back();
    }
    size_t lastFirst = mu::kNoTrack;
    for (size_t i = 0; i < use.size(); ++i)
        if (use[i].key == lastKey) lastFirst = i;
    static std::mt19937 rng{std::random_device{}()};
    const std::vector<size_t> order = mu::ShuffleOrder(use.size(), lastFirst, rng);

    std::vector<const mu::Track*> tracks;
    for (size_t i : order) tracks.push_back(use[i].track);
    std::vector<uint8_t> bank;
    std::string err;
    if (!mu::BuildBank(vanilla->data(), tracks, &bank, &err)) {
        LOG_ERROR("[music] %s: the bank could not be built: %s", kSlot, err.c_str());
        SetRedirect({});
        return;
    }
    if (!WriteAtomic(BankPath(), bank)) {
        // The game may still hold the previous bank open; the old one stays in place (and in use) and we try again later.
        LOG_WARN("[music] %s: %s could not be written; keeping the previous bank", kSlot, kRel);
        if (g_retries++ < kMaxRetries) g_retryIn = kRetryFrames;
        return;
    }
    g_retries = 0;
    SetRedirect(BankPath().wstring());
    {
        std::ofstream f(lastPath, std::ios::trunc);
        f << use[order[0]].key << "\n";
    }
    uint64_t samples = 0;
    std::string mods, titles;
    for (size_t i : order) {
        samples += use[i].track->samples;
        titles += (titles.empty() ? "" : ", ") + use[i].title;
    }
    for (const auto& it : use)
        if (mods.find(it.mod) == std::string::npos) mods += (mods.empty() ? "" : ", ") + it.mod;
    LOG_INFO("[music] %s: %zu tracks from %s, order %s, %s, bank %s", kSlot, use.size(), mods.c_str(), titles.c_str(),
             Duration(samples, first->sampleRate).c_str(), kRel);
    for (const auto& it : use)
        if (!it.credit.empty()) LOG_INFO("[music]   %s: %s (%s)", it.key.c_str(), it.title.c_str(), it.credit.c_str());
    melange::jlog::Rec("music", melange::jlog::Level::Info, "bank").Str("slot", kSlot).Uint("tracks", static_cast<uint64_t>(use.size()));
}

void OnFrame() {
    const bool front = leng::AtFrontend();
    if (front && !g_wasFront) g_dirty = true;       // back from a match: the next one gets a new order
    g_wasFront = front;
    g_frontendFrames = front ? g_frontendFrames + 1 : 0;
    if (g_retryIn > 0 && --g_retryIn == 0) g_dirty = true;
    if (!front || g_frontendFrames < kSettleFrames || !weng::AppReady() || !weng::Drm()) return;
    if (g_dirty) Build();
}

class Music final : public melange::Module {
public:
    const char* Name() const override { return "Music"; }
    const char* Description() const override { return "replaces the sudden-death music with tracks from mods"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 57; }
    bool Install() override {
        // kernel32's CreateFileA, which FMOD opens its banks with. There is no byte pattern to compare (it differs by
        // Windows version), so the check is that the export resolves and the inline hook was created.
        void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileA"));
        if (!target) {
            LOG_ERROR("[music] kernel32!CreateFileA not found; disabled");
            return false;
        }
        g_hook = safetyhook::create_inline(target, reinterpret_cast<void*>(&HkCreateFileA));
        if (!g_hook) {
            LOG_ERROR("[music] hooking kernel32!CreateFileA at %p failed; disabled", target);
            return false;
        }
        g_hookOk = true;
        melange::mods::OnChange([](void*) { g_dirty = true; }, nullptr);
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        LOG_INFO("[music] installed");
        return true;
    }
    void Uninstall() override {
        g_hookOk = false;
        g_hook = {};
    }
};
}  // namespace

MELANGE_MODULE(Music);
