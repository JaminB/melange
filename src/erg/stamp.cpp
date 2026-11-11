#include "erg/stamp.h"

#include <windows.h>

#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>

#include "erg/install.h"
#include "levels/manifest.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::erg::stamp {
namespace {
std::wstring Native(const std::wstring& dir, const std::string& rel) {
    std::wstring out = dir + L"\\" + install::Widen(rel);
    for (wchar_t& c : out)
        if (c == L'/') c = L'\\';
    return out;
}

bool Stat(const std::wstring& path, uint64_t* size, uint64_t* time) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    *size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    *time = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    return true;
}

// sha256 of a file, cached by path, size and write time: Check runs at every launcher start and before every launch.
std::string FileHash(const std::wstring& path) {
    static std::mutex mx;
    struct Entry {
        uint64_t size = 0, time = 0;
        std::string sha;
    };
    static std::map<std::wstring, Entry> cache;
    uint64_t size = 0, time = 0;
    if (!Stat(path, &size, &time)) return "";
    {
        std::lock_guard lk(mx);
        auto it = cache.find(path);
        if (it != cache.end() && it->second.size == size && it->second.time == time) return it->second.sha;
    }
    const std::string sha = hashutil::Sha256HexFile(path);
    if (sha.empty()) return "";
    std::lock_guard lk(mx);
    cache[path] = {size, time, sha};
    return sha;
}

void Add(hashutil::Sha256* h, const std::string& s) {
    const std::string line = s + "\n";
    h->Update(line.data(), line.size());
}
}  // namespace

const char* Name(State s) {
    switch (s) {
        case State::None: return "none";
        case State::Built: return "built";
        case State::Missing: return "missing";
        case State::Stale: return "stale";
        case State::Failed: return "failed";
    }
    return "none";
}

bool HasSources(const spice::Manifest& m) {
    for (const auto& l : m.levels)
        if (!l.source.empty()) return true;
    return false;
}

bool Inputs(const std::wstring& modDir, const spice::Manifest& m, const std::string& melangeVersion, const std::wstring& gameDir,
            std::string* hash, std::string* err) {
    hashutil::Sha256 h;
    Add(&h, "ergbuild/1");
    Add(&h, "melange " + melangeVersion);
    // The game: its exe's size tells one build of the game from another (its write time changes with the 4 GB switch).
    uint64_t size = 0, time = 0;
    Add(&h, "game " + (Stat(gameDir + L"\\WormsMayhem.exe", &size, &time) ? std::to_string(size) : std::string("-")));
    const std::string spiceSha = FileHash(modDir + L"\\spice.json");
    if (spiceSha.empty()) return *err = "spice.json cannot be read", false;
    Add(&h, "spice.json " + spiceSha);
    for (const auto& l : m.levels) {
        if (l.source.empty()) continue;
        const std::string sha = FileHash(Native(modDir, l.source));
        if (sha.empty()) return *err = l.slug + ": " + l.source + " cannot be read", false;
        Add(&h, "level " + l.slug + " " + sha);
    }
    for (const auto& mesh : m.meshes) {
        std::string sha = FileHash(Native(modDir, mesh.file));
        if (sha.empty()) sha = FileHash(Native(modDir + L"\\" + install::Widen(m.assetsRoot), mesh.file));
        Add(&h, "mesh " + mesh.file + " " + (sha.empty() ? std::string("-") : sha));
    }
    *hash = h.FinishHex();
    if (hash->empty()) return *err = "sha256 failed", false;
    return true;
}

bool Read(const std::wstring& modDir, Record* out) {
    *out = Record{};
    json::Value v;
    json::Error e;
    if (!json::ParseFile(modDir + L"\\" + kFile, &v, &e, 64 * 1024) || !v.IsObject()) return false;
    auto s = [&](const char* k) {
        const json::Value* x = v.Get(k);
        return x && x->IsString() ? x->string : std::string();
    };
    const json::Value* ok = v.Get("ok");
    out->ok = ok && ok->IsBool() && ok->boolean;
    out->inputs = s("inputs");
    out->melange = s("melange");
    out->error = s("error");
    out->at = s("at");
    return !out->inputs.empty();
}

bool Write(const std::wstring& modDir, const Record& r) {
    jsonmini::Obj o;
    o.Int("version", 1).Bool("ok", r.ok).Str("inputs", r.inputs).Str("melange", r.melange).Str("at", r.at);
    if (!r.error.empty()) o.Str("error", r.error);
    const std::string text = o.End() + "\n";
    std::string err;
    return install::WriteAtomic(modDir + L"\\" + kFile, text.data(), text.size(), &err);
}

std::string Now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

State Check(const std::wstring& modDir, const spice::Manifest& m, const std::string& melangeVersion, const std::wstring& gameDir,
            std::string* inputs, std::string* why) {
    inputs->clear();
    why->clear();
    if (!HasSources(m)) return State::None;
    std::string err;
    if (!Inputs(modDir, m, melangeVersion, gameDir, inputs, &err)) {
        *why = err;
        return State::Failed;
    }
    Record rec;
    const bool have = Read(modDir, &rec);
    if (have && rec.inputs == *inputs && !rec.ok) {
        *why = rec.error.empty() ? "the last build failed" : rec.error;
        return State::Failed;
    }
    install::Pack pack;
    if (!install::PackFromManifest(m, modDir, &pack)) {
        *why = "its levels are refused";
        return State::Failed;
    }
    const std::wstring root = install::LevelRoot(pack);
    for (const auto& d : pack.levels) {
        if (d.source.empty()) continue;
        for (const auto& f : levels::manifest::RequiredFiles(d))
            if (!install::Exists(Native(root, f))) {
                *why = "levels/" + f + " is missing";
                return State::Missing;
            }
    }
    if (!have) {
        *why = "no build stamp";
        return State::Missing;
    }
    if (rec.inputs != *inputs) {
        *why = rec.melange != melangeVersion ? "built by Melange " + rec.melange : "the pack changed since it was built";
        return State::Stale;
    }
    return State::Built;
}
}  // namespace melange::erg::stamp
