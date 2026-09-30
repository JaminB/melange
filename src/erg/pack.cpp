#include "erg/pack.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "erg/names.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace fs = std::filesystem;

namespace melange::erg::pack {
namespace {
bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

bool NoReparse(const fs::path& dir, const fs::path& rel) {
    auto plain = [](const fs::path& p) {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT);
    };
    fs::path cur = dir;
    if (!plain(cur)) return false;
    for (const auto& part : rel)
        if (!plain(cur /= part)) return false;
    return true;
}

// A round-trip serializer for the generic json::Value DOM (json_read.h has no writer of its own): used only to
// preserve an existing spice.json's other levels[] entries byte-for-byte across a re-export.
std::string Serialize(const json::Value& v) {
    switch (v.type) {
        case json::Type::Null: return "null";
        case json::Type::Bool: return v.boolean ? "true" : "false";
        case json::Type::Number: {
            if (v.IsInteger()) return std::to_string(static_cast<int64_t>(v.number));
            char buf[48];
            snprintf(buf, sizeof buf, "%.17g", v.number);
            return buf;
        }
        case json::Type::String: return "\"" + jsonmini::Escape(v.string) + "\"";
        case json::Type::Array: {
            jsonmini::Arr a;
            for (auto& it : v.items) a.Raw(Serialize(it));
            return a.End();
        }
        case json::Type::Object: {
            jsonmini::Obj o;
            for (auto& [k, mv] : v.members) o.Raw(k, Serialize(mv));
            return o.End();
        }
    }
    return "null";
}

struct Existing {
    bool present = false;
    std::string id, name, version;
    std::vector<json::Value> levels;  // other slugs' entries, preserved verbatim
};

// Reads a folder's spice.json, if any, as a manifest Erg could have written: only the fields a level-only content
// mod needs. Any other top-level key (weapons, settings, an entry point, ...) means the folder holds a mod Erg did
// not create, and the export is refused rather than risk clobbering it.
bool ReadExisting(const std::wstring& dir, const std::string& slug, Existing* out, std::string* err) {
    const std::wstring path = dir + L"\\spice.json";
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (fs::exists(dir, ec))
            for (auto& e : fs::directory_iterator(dir, ec)) {
                (void)e;
                return Fail(err, "this folder already holds files but no spice.json ('" + e.path().filename().string() + "')");
            }
        return true;
    }
    json::Value v;
    json::Error e;
    if (!json::ParseFile(path, &v, &e) || !v.IsObject())
        return Fail(err, "an existing spice.json in this folder does not parse");
    static constexpr std::string_view kKnown[] = {"spiceVersion", "id", "version", "name", "authors", "description",
                                                   "melange", "kind", "defaultEnabled", "levels"};
    for (auto& [k, mv] : v.members) {
        (void)mv;
        if (std::find(std::begin(kKnown), std::end(kKnown), std::string_view(k)) == std::end(kKnown))
            return Fail(err, "this folder holds a mod Erg did not create (spice.json has '" + k + "')");
    }
    const json::Value* kind = v.Get("kind");
    if (!kind || !kind->IsString() || kind->string != "content")
        return Fail(err, "this folder holds a mod Erg did not create (not a content mod)");
    out->present = true;
    if (auto* m = v.Get("id")) out->id = m->string;
    if (auto* m = v.Get("name")) out->name = m->string;
    if (auto* m = v.Get("version")) out->version = m->string;
    if (auto* m = v.Get("levels"))
        if (m->IsArray())
            for (auto& item : m->items) {
                const json::Value* s = item.Get("slug");
                if (s && s->IsString() && s->string == slug) continue;  // this level: replaced below
                out->levels.push_back(item);
            }
    return true;
}

bool WriteFile(const fs::path& path, const void* data, size_t n, std::string* err) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return Fail(err, "could not write '" + path.string() + "'");
    if (n) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return f.good() || n == 0 ? true : Fail(err, "write failed for '" + path.string() + "'");
}

// '/' -> native separators, and refuses '..' or a rooted path (files never escape the pack folder).
bool SafeRel(const std::string& rel, fs::path* out) {
    if (rel.empty() || rel.front() == '/' || rel.find("..") != std::string::npos) return false;
    std::string native = rel;
    std::replace(native.begin(), native.end(), '/', static_cast<char>(fs::path::preferred_separator));
    *out = native;
    return true;
}
}  // namespace

bool WritePack(const PackSpec& spec, const std::wstring& dir, std::vector<std::string>* files, std::string* err) {
    if (files) files->clear();
    if (spec.modId.empty() || spec.name.empty() || spec.version.empty())
        return Fail(err, "modId, name and version are required");
    std::string why;
    const std::string prefix = names::Prefix(spec.modId);
    if (!names::ValidPrefix(prefix, &why)) return Fail(err, why);
    if (!names::ValidSlug(spec.slug)) return Fail(err, "the slug '" + spec.slug + "' must match [a-z0-9]{1,24}");
    const std::string stem = prefix + "_" + spec.slug;
    if (!names::ValidStem(stem, prefix, &why)) return Fail(err, why);

    Existing existing;
    if (!ReadExisting(dir, spec.slug, &existing, err)) return false;
    if (existing.present && !existing.id.empty() && existing.id != spec.modId)
        return Fail(err, "this folder already holds mod '" + existing.id + "'");

    // The level's own manifest entry, plus every other level this mod already had.
    const std::string lubRel = "assets/levels/" + stem + ".lub";
    const bool chunk = spec.source ? spec.chunk : std::any_of(spec.levelFiles.begin(), spec.levelFiles.end(),
                                    [&](const File& f) { return f.rel == lubRel; });
    jsonmini::Obj entry;
    entry.Str("slug", spec.slug).Str("title", spec.title.empty() ? spec.name : spec.title).Str("type", "multi").Bool("chunk", chunk);
    entry.Str("source", "src/" + spec.slug + ".ergpatch.json");
    jsonmini::Arr levels;
    levels.Raw(entry.End());
    for (auto& lv : existing.levels) levels.Raw(Serialize(lv));

    jsonmini::Obj manifest;
    manifest.Int("spiceVersion", 1).Str("id", spec.modId).Str("version", spec.version);
    manifest.Str("name", existing.present && !existing.name.empty() ? existing.name : spec.name);
    manifest.Raw("melange", R"({"range":">=0.2.0"})");
    manifest.Str("kind", "content").Bool("defaultEnabled", false);
    manifest.Raw("levels", levels.End());
    const std::string manifestJson = manifest.End();

    std::vector<std::pair<fs::path, std::string>> pending;  // path, textual note for `files`
    pending.emplace_back(fs::path(L"spice.json"), "spice.json");

    if (!spec.source) {
        for (auto& f : spec.levelFiles) {
            if (f.rel.size() >= 4 && f.rel.compare(f.rel.size() - 4, 4, ".csh") == 0)
                continue;  // hard rule: a shadow cache file is never shipped
            fs::path rel;
            if (!SafeRel(f.rel, &rel)) return Fail(err, "unsafe pack file path '" + f.rel + "'");
            pending.emplace_back(rel, f.rel);
        }
    }
    const fs::path patchRel(L"src\\" + std::wstring(spec.slug.begin(), spec.slug.end()) + L".ergpatch.json");
    pending.emplace_back(patchRel, "src/" + spec.slug + ".ergpatch.json");

    if (spec.source) {
        pending.emplace_back(fs::path(L"build.ps1"), "build.ps1");
        pending.emplace_back(fs::path(L".gitignore"), ".gitignore");
    }

    // Validate everything before writing anything, so a bad spec never leaves a half-written pack.
    for (auto& [rel, label] : pending) {
        if (rel.empty()) return Fail(err, "empty path for '" + label + "'");
        if (!NoReparse(dir, rel)) return Fail(err, "'" + label + "' would be written through a link or junction");
    }
    std::vector<fs::path> stale;
    if (!spec.source)
        for (const std::string rel : {"assets/levels/Maps/" + stem + ".hmp", "assets/levels/Maps/" + stem + ".txt", lubRel}) {
            fs::path p;
            if (std::none_of(spec.levelFiles.begin(), spec.levelFiles.end(), [&](const File& f) { return f.rel == rel; }) &&
                SafeRel(rel, &p) && NoReparse(dir, p))
                stale.push_back(fs::path(dir) / p);
        }

    std::vector<std::string> written;
    for (auto& [rel, label] : pending) {
        const fs::path full = fs::path(dir) / rel;
        std::string bytes;
        if (label == "spice.json") {
            bytes = manifestJson;
        } else if (label == "src/" + spec.slug + ".ergpatch.json") {
            bytes = spec.patchJson;
        } else if (label == "build.ps1") {
            const std::string patchArg = "src\\" + spec.slug + ".ergpatch.json";
            bytes = "# Regenerates assets\\levels from " + patchArg +
                    " against this machine's own install.\n"
                    "# Usage: .\\build.ps1 [-Game <game folder>] [-XomTool <xomtool.exe>]\n"
                    "param([string]$Game = \"\", [string]$XomTool = \"\")\n"
                    "$ErrorActionPreference = \"Stop\"\n"
                    "$root = $PSScriptRoot\n"
                    "if (-not $Game) { $Game = Split-Path (Split-Path $root -Parent) -Parent }\n"
                    "if (-not (Test-Path (Join-Path $Game \"Data\\Maps\"))) { throw \"$Game is not a game folder (no "
                    "Data\\Maps); pass -Game <game folder>\" }\n"
                    "if (-not $XomTool) {\n"
                    "    $cmd = Get-Command xomtool -ErrorAction SilentlyContinue\n"
                    "    if ($cmd) { $XomTool = $cmd.Source }\n"
                    "}\n"
                    "if (-not $XomTool -or -not (Test-Path $XomTool)) {\n"
                    "    throw \"xomtool.exe not found; pass -XomTool <path>, or put it on PATH (it ships at "
                    "dist\\tools\\xomtool.exe in the melange repo)\"\n"
                    "}\n"
                    "& $XomTool level build --patch (Join-Path $root \"" + patchArg +
                    "\") --game $Game --out (Join-Path $root \"assets\\levels\")\n"
                    "if ($LASTEXITCODE) { throw \"xomtool level build failed\" }\n";
        } else if (label == ".gitignore") {
            bytes = "assets/\n";
        } else {
            const auto it = std::find_if(spec.levelFiles.begin(), spec.levelFiles.end(),
                                          [&](const File& f) { return f.rel == label; });
            if (it == spec.levelFiles.end()) return Fail(err, "internal: missing bytes for '" + label + "'");
            if (!WriteFile(full, it->bytes.data(), it->bytes.size(), err)) return false;
            written.push_back(label);
            continue;
        }
        if (!WriteFile(full, bytes.data(), bytes.size(), err)) return false;
        written.push_back(label);
    }
    for (const auto& p : stale) {
        std::error_code ec;
        fs::remove(p, ec);
    }
    if (files) *files = written;
    return true;
}
}  // namespace melange::erg::pack
