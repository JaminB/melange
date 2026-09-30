// D: asset previews. Detail meshes and theme materials are pure file conversions over src/xom (Data\Bundles,
// Data\Themes) with no game-memory access, cached on disk under Documents\Melange\erg\cache\<converter>\<key>.
#include "erg/preview.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "erg/scene.h"
#include "melange/jlog.h"
#include "core/config.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "xom/gltf.h"
#include "xom/image.h"
#include "xom/json.h"
#include "xom/mesh.h"
#include "xom/xom.h"

namespace melange::erg::preview {
namespace {
namespace fs = std::filesystem;
using xom::Document;
using xom::Json;
using xom::Object;

// ---------------------------------------------------------------- small helpers

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}
// s is always plain ASCII here (a theme folder/file name we built ourselves).
std::wstring ToWide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string BaseName(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}
std::string StripExt(std::string_view name) {
    const size_t dot = name.find_last_of('.');
    return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}
// A key's own segments are restricted to this charset so it is always a safe URL path segment and cache file name.
bool SafeSegment(std::string_view s) {
    if (s.empty() || s.size() > 96) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-')) return false;
    return s.find("..") == std::string_view::npos;
}

bool ReadAll(const fs::path& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = static_cast<std::streamoff>(f.tellg());
    if (n < 0) return false;
    out->resize(static_cast<size_t>(n));
    f.seekg(0);
    if (!out->empty()) f.read(reinterpret_cast<char*>(out->data()), std::streamsize(out->size()));
    return bool(f) || f.eof();
}
bool ReadAllText(const fs::path& p, std::string* out) {
    std::vector<uint8_t> bytes;
    if (!ReadAll(p, &bytes)) return false;
    out->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}
bool WriteAll(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (n) f.write(reinterpret_cast<const char*>(data), std::streamsize(n));
    return bool(f);
}

Json* FindMutable(Json& obj, std::string_view key) {
    for (auto& kv : obj.obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

// ---------------------------------------------------------------- game dir and well-known folders

std::mutex g_gameDirMx;
std::wstring g_gameDir;

void SetGameDirImpl(std::wstring dir) {
    std::lock_guard lk(g_gameDirMx);
    g_gameDir = std::move(dir);
}
std::wstring GameDirNow() {
    std::lock_guard lk(g_gameDirMx);
    return g_gameDir;
}

std::wstring DocumentsDir() {
    PWSTR p = nullptr;
    std::wstring docs = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) docs = p;
    if (p) CoTaskMemFree(p);
    return docs;
}
fs::path CacheRoot() { return fs::path(DocumentsDir()) / L"Melange" / L"erg" / L"cache"; }

// ---------------------------------------------------------------- the bundle index (lazy, built once)

struct MeshLoc {
    std::wstring bundleFile;  // e.g. L"Bundl24.xom", relative to Data\Bundles
    std::string resourceId;   // exact original casing, as mesh::ReadMesh needs
};
struct ImageLoc {
    std::wstring bundleFile;
    uint32_t ref = 0;  // 1-based object index within that bundle, once re-parsed
};

struct Index {
    std::mutex mx;
    bool built = false, buildFailed = false;
    std::unordered_map<std::string, MeshLoc> meshesByLowerId;      // lower(ResourceId) -> location
    std::unordered_map<std::string, ImageLoc> imagesByLowerStem;   // lower(Name without extension) -> location
};
Index& GetIndex() {
    static Index idx;
    return idx;
}

// Scans every Data\Bundles\*.xom once; holds the lock for the whole scan (a few hundred ms, once per process).
bool EnsureIndexBuilt(Index& idx) {
    std::lock_guard lk(idx.mx);
    if (idx.built) return true;
    if (idx.buildFailed) return false;
    const std::wstring gameDir = GameDirNow();
    if (gameDir.empty()) return false;
    const fs::path bundlesDir = fs::path(gameDir) / L"Data" / L"Bundles";
    std::error_code ec;
    if (!fs::is_directory(bundlesDir, ec) || ec) {
        idx.buildFailed = true;
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t files = 0, meshes = 0, images = 0;
    for (const auto& entry : fs::directory_iterator(bundlesDir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        if (Lower(p.extension().string()) != ".xom") continue;
        std::vector<uint8_t> bytes;
        if (!ReadAll(p, &bytes)) continue;
        Document doc;
        std::string err;
        if (!xom::parse(bytes.data(), bytes.size(), doc, &err)) continue;
        ++files;
        const std::wstring rel = p.filename().wstring();
        for (size_t i = 0; i < doc.objects.size(); ++i) {
            const Object& o = doc.objects[i];
            if (o.type == "XMeshDescriptor") {
                const xom::Value* rid = o.field("ResourceId");
                if (rid && !rid->str.empty()) {
                    idx.meshesByLowerId.emplace(Lower(rid->str), MeshLoc{rel, rid->str});
                    ++meshes;
                }
            } else if (o.type == "XImage") {
                const xom::Value* nm = o.field("Name");
                if (nm && !nm->str.empty()) {
                    idx.imagesByLowerStem.emplace(Lower(StripExt(nm->str)), ImageLoc{rel, uint32_t(i + 1)});
                    ++images;
                }
            }
        }
    }
    idx.built = files > 0;
    if (!idx.built) idx.buildFailed = true;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    jlog::Rec("erg", jlog::Level::Info, "preview_index")
        .Uint("files", files).Uint("meshes", meshes).Uint("images", images).Float("ms", ms);
    return idx.built;
}

// ---------------------------------------------------------------- limiting concurrent conversions to 2

std::mutex g_slotMx;
std::condition_variable g_slotCv;
int g_slotsInUse = 0;
constexpr int kMaxConcurrentConversions = 2;
struct Slot {
    Slot() {
        std::unique_lock lk(g_slotMx);
        g_slotCv.wait(lk, [] { return g_slotsInUse < kMaxConcurrentConversions; });
        ++g_slotsInUse;
    }
    ~Slot() {
        {
            std::lock_guard lk(g_slotMx);
            --g_slotsInUse;
        }
        g_slotCv.notify_one();
    }
};

// ---------------------------------------------------------------- stats and the in-memory "failed" cache

std::mutex g_statsMx;
Stats g_stats;
std::unordered_map<std::string, std::string> g_failedThisSession;  // key -> error, cleared never (process lifetime)

// ---------------------------------------------------------------- glTF post-processing: pack to .glb, one flat
// baseColorFactor material shared by every primitive (gltf.h itself writes geometry only, no materials/images).

std::vector<uint8_t> PackGlb(std::string json, std::vector<uint8_t> bin) {
    auto pad = [](size_t n) { return (4 - n % 4) % 4; };
    json.append(pad(json.size()), ' ');
    bin.resize(bin.size() + pad(bin.size()), 0);
    std::vector<uint8_t> out;
    out.reserve(28 + json.size() + bin.size());
    auto put32 = [&](uint32_t v) {
        out.push_back(uint8_t(v));
        out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >> 24));
    };
    const uint32_t total = uint32_t(12 + 8 + json.size() + 8 + bin.size());
    put32(0x46546C67u);  // "glTF"
    put32(2u);
    put32(total);
    put32(uint32_t(json.size()));
    put32(0x4E4F534Au);  // "JSON"
    out.insert(out.end(), json.begin(), json.end());
    put32(uint32_t(bin.size()));
    put32(0x004E4942u);  // "BIN\0"
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

// baseColor: linear RGB 0..1 shared by every mesh/primitive; embedding the buffer (dropping "uri") makes the
// document self-contained, as a .glb needs.
bool AddFlatMaterial(std::string& json, const std::array<double, 3>& baseColor, std::string* error) {
    Json root;
    if (!xom::ParseJson(json, root, error)) return false;
    Json* buffers = FindMutable(root, "buffers");
    if (buffers && !buffers->arr.empty()) {
        auto& fields = buffers->arr[0].obj;
        fields.erase(std::remove_if(fields.begin(), fields.end(), [](auto& kv) { return kv.first == "uri"; }),
                     fields.end());
    }
    Json* meshesJ = FindMutable(root, "meshes");
    Json materials = Json::Arr();
    Json matJ = Json::Obj();
    Json pbr = Json::Obj();
    Json factor = Json::Arr();
    factor.arr.push_back(Json::Num(baseColor[0]));
    factor.arr.push_back(Json::Num(baseColor[1]));
    factor.arr.push_back(Json::Num(baseColor[2]));
    factor.arr.push_back(Json::Num(1.0));
    pbr.set("baseColorFactor", std::move(factor));
    pbr.set("metallicFactor", Json::Num(0.0));
    pbr.set("roughnessFactor", Json::Num(0.9));
    matJ.set("pbrMetallicRoughness", std::move(pbr));
    materials.arr.push_back(std::move(matJ));
    root.set("materials", std::move(materials));
    if (meshesJ) {
        for (auto& meshEntry : meshesJ->arr) {
            Json* prims = FindMutable(meshEntry, "primitives");
            if (!prims) continue;
            for (auto& prim : prims->arr) prim.set("material", Json::Int(0));
        }
    }
    json = xom::WriteJson(root);
    return true;
}

// sRGB 0..255 average -> linear 0..1, the space glTF's baseColorFactor wants.
std::array<double, 3> SrgbToLinear(const std::array<uint8_t, 3>& c) {
    std::array<double, 3> out{};
    for (int i = 0; i < 3; ++i) {
        const double s = c[size_t(i)] / 255.0;
        out[size_t(i)] = s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    }
    return out;
}

std::array<uint8_t, 3> AverageColor(const xom::image::Pixels& px) {
    if (px.width == 0 || px.height == 0 || px.data.empty()) return {128, 128, 128};
    uint64_t r = 0, g = 0, b = 0;
    const size_t n = size_t(px.width) * px.height;
    for (size_t i = 0; i < n; ++i) {
        const size_t o = i * size_t(px.channels);
        r += px.data[o];
        g += px.data[o + 1];
        b += px.data[o + 2];
    }
    return {uint8_t(r / n), uint8_t(g / n), uint8_t(b / n)};
}

// The smallest available mip (cheap to average) of a resolved XImage; false if it has no readable mip.
bool SmallestMip(const Object& img, xom::image::Pixels* out, std::string* error) {
    const xom::Value* mips = img.field("MipLevels");
    const int count = mips ? int(mips->asUInt()) : 1;
    for (int lvl = std::max(0, count - 1); lvl >= 0; --lvl)
        if (xom::image::ExtractMip(img, lvl, *out, error)) return true;
    return false;
}

// ---------------------------------------------------------------- mesh conversion

bool ConvertMesh(const MeshLoc& loc, std::vector<uint8_t>* glb, std::string* error) {
    const fs::path bundlePath = fs::path(GameDirNow()) / L"Data" / L"Bundles" / loc.bundleFile;
    std::vector<uint8_t> bytes;
    if (!ReadAll(bundlePath, &bytes)) {
        if (error) *error = "cannot read the bundle";
        return false;
    }
    Document doc;
    if (!xom::parse(bytes.data(), bytes.size(), doc, error)) return false;

    xom::mesh::Mesh mesh;
    if (!xom::mesh::ReadMesh(doc, loc.resourceId, mesh, error)) return false;
    if (mesh.primitives.empty()) {
        if (error) *error = "no primitives";
        return false;
    }

    std::array<double, 3> color{0.6, 0.6, 0.6};
    std::string ignore;
    const uint32_t shaderRef = xom::mesh::FindMeshShader(doc, loc.resourceId, &ignore);
    if (shaderRef) {
        const uint32_t texRef = xom::mesh::FindShaderTexture(doc, shaderRef, &ignore);
        const Object* texObj = texRef ? doc.object(texRef) : nullptr;
        xom::image::Pixels px;
        if (texObj && SmallestMip(*texObj, &px, &ignore)) color = SrgbToLinear(AverageColor(px));
    }

    auto out = xom::gltf::WriteGltf(mesh.primitives, "mesh.bin");
    if (!AddFlatMaterial(out.json, color, error)) return false;
    *glb = PackGlb(std::move(out.json), std::move(out.bin));
    return true;
}

// ---------------------------------------------------------------- theme material atlas

constexpr int kAtlasCell = 16, kAtlasCols = 8, kAtlasRows = 8, kAtlasMaxMaterials = kAtlasCols * kAtlasRows;

std::string ThemeFolder(const std::string& theme) {
    // "CAMELOT" -> "ThemeCamelot": every fixed theme name is one word (erg::ValidTheme's table).
    std::string t = Lower(theme);
    if (!t.empty()) t[0] = char(std::toupper(static_cast<unsigned char>(t[0])));
    return "Theme" + t;
}

struct MaterialRecord {
    std::string textureRef;  // as the file names it, e.g. "BeigeRock/Grass01"; "" if the slot has none (NULL)
};

// The theme's <ThemeName>.txt: 6-line records separated by blank lines; line index 4 (5th line) names a texture
// (or "NULL"). Capped at kAtlasMaxMaterials (the voxel material field is 6 bits).
bool ParseMaterialFile(const std::string& text, std::vector<MaterialRecord>* out) {
    std::vector<std::string> block;
    auto flush = [&] {
        if (!block.empty()) {
            MaterialRecord r;
            if (block.size() > 4) {
                const std::string tex = Trim(block[4]);
                if (!tex.empty() && tex != "NULL") r.textureRef = tex;
            }
            out->push_back(std::move(r));
        }
        block.clear();
    };
    size_t i = 0;
    while (i <= text.size()) {
        const size_t nl = text.find('\n', i);
        std::string line = text.substr(i, nl == std::string::npos ? text.size() - i : nl - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (Trim(line).empty()) {
            flush();
        } else {
            block.push_back(std::move(line));
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    flush();
    if (out->size() > size_t(kAtlasMaxMaterials)) out->resize(size_t(kAtlasMaxMaterials));
    return !out->empty();
}

bool LoadThemeMaterials(const std::string& theme, std::vector<MaterialRecord>* out, std::string* error) {
    const std::string themeFolder = ThemeFolder(theme);
    const fs::path folder = fs::path(GameDirNow()) / L"Data" / L"Themes" / ToWide(themeFolder);
    const fs::path file = folder / ToWide(themeFolder + ".txt");
    std::string text;
    if (!ReadAllText(file, &text)) {
        if (error) *error = "cannot read the theme's material file";
        return false;
    }
    if (!ParseMaterialFile(text, out)) {
        if (error) *error = "the material file has no records";
        return false;
    }
    return true;
}

// A texture named by a material record resolves through the same bundle index as meshes; unresolved slots (or a
// slot with no texture) fall back to a stable colour derived from the record's own text, so the atlas always has
// exactly as many cells as materials and never fails outright.
std::array<uint8_t, 3> FallbackColor(const std::string& seed) {
    const std::string h = hashutil::Sha256Hex(seed.data(), seed.size());
    auto hex2 = [&](size_t i) { return uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)); };
    // Keep it mid-tone (not too dark/bright) so a missing texture is visually distinct from a resolved one.
    return {uint8_t(80 + hex2(0) % 120), uint8_t(80 + hex2(2) % 120), uint8_t(80 + hex2(4) % 120)};
}

bool ConvertThemeAtlas(const std::string& theme, Index& idx, std::vector<uint8_t>* png,
                        std::vector<bool>* resolvedOut, std::string* error) {
    std::vector<MaterialRecord> materials;
    if (!LoadThemeMaterials(theme, &materials, error)) return false;

    std::vector<std::array<uint8_t, 3>> colors(materials.size());
    resolvedOut->assign(materials.size(), false);
    for (size_t i = 0; i < materials.size(); ++i) {
        const std::string& ref = materials[i].textureRef;
        bool resolved = false;
        if (!ref.empty()) {
            const auto it = idx.imagesByLowerStem.find(Lower(StripExt(BaseName(ref))));
            if (it != idx.imagesByLowerStem.end()) {
                const fs::path bundlePath = fs::path(GameDirNow()) / L"Data" / L"Bundles" / it->second.bundleFile;
                std::vector<uint8_t> bytes;
                Document doc;
                std::string ignore;
                if (ReadAll(bundlePath, &bytes) && xom::parse(bytes.data(), bytes.size(), doc, &ignore)) {
                    const Object* img = doc.object(it->second.ref);
                    xom::image::Pixels px;
                    if (img && img->type == "XImage" && SmallestMip(*img, &px, &ignore)) {
                        colors[i] = AverageColor(px);
                        resolved = true;
                    }
                }
            }
        }
        if (!resolved) colors[i] = FallbackColor(theme + "|" + std::to_string(i) + "|" + ref);
        (*resolvedOut)[i] = resolved;
    }

    xom::image::Pixels atlas;
    atlas.width = uint16_t(kAtlasCols * kAtlasCell);
    atlas.height = uint16_t(kAtlasRows * kAtlasCell);
    atlas.channels = 3;
    atlas.data.assign(size_t(atlas.width) * atlas.height * 3, 24);
    for (size_t i = 0; i < colors.size(); ++i) {
        const int cx = int(i) % kAtlasCols, cy = int(i) / kAtlasCols;
        for (int y = 0; y < kAtlasCell; ++y) {
            for (int x = 0; x < kAtlasCell; ++x) {
                const size_t o = (size_t(cy * kAtlasCell + y) * atlas.width + size_t(cx * kAtlasCell + x)) * 3;
                atlas.data[o] = colors[i][0];
                atlas.data[o + 1] = colors[i][1];
                atlas.data[o + 2] = colors[i][2];
            }
        }
    }

    int len = 0;
    unsigned char* mem =
        stbi_write_png_to_mem(atlas.data.data(), atlas.width * atlas.channels, atlas.width, atlas.height, atlas.channels, &len);
    if (!mem) {
        if (error) *error = "PNG encode failed";
        return false;
    }
    png->assign(mem, mem + len);
    STBIW_FREE(mem);
    return true;
}

// ---------------------------------------------------------------- cache (Documents\Melange\erg\cache\<conv>\<key>)

fs::path CacheFile(std::string_view converter, std::string_view sha, std::string_view ext) {
    std::string name(sha);
    name += '.';
    name += ext;
    return CacheRoot() / std::string(converter) / name;
}

bool CacheRead(const fs::path& p, std::vector<uint8_t>* out) {
    if (!ReadAll(p, out)) return false;
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);  // mark as recently used for TrimCache's LRU
    return true;
}

}  // namespace

bool Available() { return true; }

void SetGameDir(std::wstring dir) { SetGameDirImpl(std::move(dir)); }

std::string DetailKey(const std::string&, const std::string& resource) {
    Index& idx = GetIndex();
    if (!EnsureIndexBuilt(idx)) return {};
    std::lock_guard lk(idx.mx);
    const auto it = idx.meshesByLowerId.find(Lower(resource));
    if (it == idx.meshesByLowerId.end()) return {};
    const std::string key = "mesh_" + Lower(resource);
    return SafeSegment(key) ? key : std::string();
}

std::string ThemeAtlasKey(const std::string& theme) {
    if (!erg::ValidTheme(theme)) return {};
    return "atlas_" + Lower(theme);
}

bool Get(const std::string& key, const std::string& ext, Asset* out, std::string* error) {
    if (!SafeSegment(key) || (ext != "glb" && ext != "png")) {
        if (error) *error = "bad key";
        return false;
    }
    {
        std::lock_guard lk(g_statsMx);
        if (const auto it = g_failedThisSession.find(key); it != g_failedThisSession.end()) {
            if (error) *error = it->second;
            return false;
        }
    }

    const bool isMesh = key.rfind("mesh_", 0) == 0;
    const bool isAtlas = key.rfind("atlas_", 0) == 0;
    if ((isMesh && ext != "glb") || (isAtlas && ext != "png") || (!isMesh && !isAtlas)) {
        if (error) *error = "key/ext mismatch";
        return false;
    }

    Index& idx = GetIndex();
    if (!EnsureIndexBuilt(idx)) {
        if (error) *error = "no game install";
        return false;
    }

    const std::string converter = isMesh ? "mesh" : "atlas";
    // The cache key covers the source bundle's own bytes, the resource, and this converter's version, so a
    // changed install or converter fix invalidates old entries automatically.
    constexpr int kConverterVersion = 1;
    std::string sourceTag;
    MeshLoc meshLoc;
    std::string themeName;
    if (isMesh) {
        const std::string resourceLower = key.substr(5);
        std::lock_guard lk(idx.mx);
        const auto it = idx.meshesByLowerId.find(resourceLower);
        if (it == idx.meshesByLowerId.end()) {
            if (error) *error = "resource not found";
            return false;
        }
        meshLoc = it->second;
        const fs::path bundlePath = fs::path(GameDirNow()) / L"Data" / L"Bundles" / meshLoc.bundleFile;
        sourceTag = hashutil::Sha256HexFile(bundlePath.wstring());
    } else {
        themeName.resize(key.size() - 6);
        std::transform(key.begin() + 6, key.end(), themeName.begin(), [](char c) { return char(std::toupper(static_cast<unsigned char>(c))); });
        if (!erg::ValidTheme(themeName)) {
            if (error) *error = "unknown theme";
            return false;
        }
        const fs::path folder = fs::path(GameDirNow()) / L"Data" / L"Themes";
        const std::string themeFolder = ThemeFolder(themeName);
        const fs::path file = folder / ToWide(themeFolder) / ToWide(themeFolder + ".txt");
        sourceTag = hashutil::Sha256HexFile(file.wstring());
    }
    if (sourceTag.empty()) {
        if (error) *error = "source file unreadable";
        return false;
    }
    const std::string cacheKeyInput = sourceTag + "|" + key + "|v" + std::to_string(kConverterVersion);
    const std::string sha = hashutil::Sha256Hex(cacheKeyInput.data(), cacheKeyInput.size());
    const fs::path cachePath = CacheFile(converter, sha, ext);

    std::vector<uint8_t> bytes;
    if (CacheRead(cachePath, &bytes)) {
        std::lock_guard lk(g_statsMx);
        ++g_stats.hits;
        out->bytes = std::move(bytes);
        out->contentType = isMesh ? "model/gltf-binary" : "image/png";
        out->etag = "\"" + sha + "\"";
        return true;
    }
    {
        std::lock_guard lk(g_statsMx);
        ++g_stats.misses;
    }

    Slot slot;  // at most kMaxConcurrentConversions conversions run at once
    const auto t0 = std::chrono::steady_clock::now();
    std::string convErr;
    bool ok;
    if (isMesh) {
        ok = ConvertMesh(meshLoc, &bytes, &convErr);
    } else {
        std::vector<bool> resolved;
        ok = ConvertThemeAtlas(themeName, idx, &bytes, &resolved, &convErr);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    if (!ok) {
        std::lock_guard lk(g_statsMx);
        ++g_stats.failed;
        g_failedThisSession[key] = convErr.empty() ? "conversion failed" : convErr;
        jlog::Rec("erg", jlog::Level::Warn, "preview_failed").Str("key", key).Str("error", convErr).Float("ms", ms);
        if (error) *error = convErr;
        return false;
    }

    WriteAll(cachePath, bytes.data(), bytes.size());
    jlog::Rec("erg", jlog::Level::Info, "preview_convert")
        .Str("key", key).Str("converter", converter).Uint("bytes", bytes.size()).Float("ms", ms);
    {
        std::lock_guard lk(g_statsMx);
        ++g_stats.conversions;
    }
    TrimCache();

    out->bytes = std::move(bytes);
    out->contentType = isMesh ? "model/gltf-binary" : "image/png";
    out->etag = "\"" + sha + "\"";
    return true;
}

std::string ThemeAtlasIndex(const std::string& theme, std::string* error) {
    if (!erg::ValidTheme(theme)) {
        if (error) *error = "unknown theme";
        return {};
    }
    std::vector<MaterialRecord> materials;
    if (!LoadThemeMaterials(theme, &materials, error)) return {};
    Index& idx = GetIndex();
    EnsureIndexBuilt(idx);  // best-effort: an unbuilt index just marks every slot unresolved below

    jsonmini::Arr arr;
    for (size_t i = 0; i < materials.size(); ++i) {
        bool resolved = false;
        if (!materials[i].textureRef.empty()) {
            std::lock_guard lk(idx.mx);
            resolved = idx.imagesByLowerStem.count(Lower(StripExt(BaseName(materials[i].textureRef)))) > 0;
        }
        arr.Raw(jsonmini::Obj()
                    .Int("index", int64_t(i))
                    .Str("name", materials[i].textureRef)
                    .Bool("resolved", resolved)
                    .End());
    }
    return jsonmini::Obj()
        .Int("cell", kAtlasCell)
        .Int("columns", kAtlasCols)
        .Int("rows", kAtlasRows)
        .Raw("materials", arr.End())
        .End();
}

void TrimCache() {
    const uint64_t budgetBytes = uint64_t(std::max(1, config::GetInt("Erg", "PreviewCacheMB", 512))) << 20;
    struct Entry { fs::path path; fs::file_time_type mtime; uint64_t bytes; };
    std::vector<Entry> entries;
    uint64_t total = 0;
    std::error_code ec;
    if (fs::is_directory(CacheRoot(), ec)) {
        for (const auto& e : fs::recursive_directory_iterator(CacheRoot(), ec)) {
            if (ec || !e.is_regular_file()) continue;
            std::error_code sizeEc, timeEc;
            const uint64_t bytes = uint64_t(e.file_size(sizeEc));
            if (sizeEc) continue;
            entries.push_back({e.path(), e.last_write_time(timeEc), bytes});
            total += bytes;
        }
    }
    if (total > budgetBytes) {
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
        for (const auto& e : entries) {
            if (total <= budgetBytes) break;
            std::error_code rmEc;
            if (fs::remove(e.path, rmEc)) total -= e.bytes;
        }
    }
    std::lock_guard lk(g_statsMx);
    g_stats.cacheBytes = total;
}

Stats GetStats() {
    std::lock_guard lk(g_statsMx);
    return g_stats;
}

}  // namespace melange::erg::preview
