#include "assets/icons.h"

#include <windows.h>

#include <cstdio>
#include <vector>

// A private, PNG-only stb_image build (mirrors postfx_gl.cpp): panelIcon is documented as a PNG, and mod content
// should never reach the other format decoders (PSD, GIF, HDR, PIC, ...) that the shared, all-formats build linked
// into draw.cpp exposes under the same symbol names. STB_IMAGE_STATIC keeps these definitions private to this
// translation unit, so the two builds coexist without a link conflict.
#pragma warning(push, 0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#pragma warning(pop)

#include "assets/upload.h"
#include "core/game.h"
#include "core/log.h"

namespace melange::assets::icons {
namespace {
constexpr int kAtlasDim = 256, kAtlasBytes = kAtlasDim * kAtlasDim * 3;
constexpr uint32_t kAtlasFmt = 0;

std::wstring ToBackslash(std::wstring s) {
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

bool SafeRel(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find_first_of("/\\", i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == "." || seg == "..") return false;
        i = j + 1;
    }
    return true;
}

struct Reservation {
    std::string modId, relPng;
    int sub;
    std::vector<uint8_t> rgba;  // kSize*kSize*4
};
std::vector<Reservation> g_reservations;
struct Decoded {
    std::string modId, relPng, err;
    std::vector<uint8_t> rgba;
};
std::vector<Decoded> g_decoded;
// Replacements for vanilla sub-icons (any of the three atlases), and the vanilla pixels kept aside for them.
struct VanillaPatch {
    std::string modId, relPng;
    int atlas, sub;
    std::vector<uint8_t> rgba;  // kSize*kSize*4
};
struct Snapshot {
    int atlas, sub;
    std::vector<uint8_t> rgb;  // kSubBytes
};
std::vector<VanillaPatch> g_vanilla;
std::vector<Snapshot> g_snaps;
constexpr int kAtlases = 3;     // "Weapon Panel Icons1".."Weapon Panel Icons3"
int g_handles[kAtlases + 1] = {};  // upload patcher per atlas, indexed 1..3
bool g_warnedShape[kAtlases + 1] = {};
bool g_clonesPatch = false;     // atlas 3 carries the clones' reserved sub-icons

bool HasSnapshot(int atlas) {
    for (auto& s : g_snaps)
        if (s.atlas == atlas) return true;
    return false;
}

bool HasVanilla(int atlas) {
    for (auto& v : g_vanilla)
        if (v.atlas == atlas) return true;
    return false;
}

void OnUpload(const char*, uint16_t w, uint16_t h, uint32_t fmt, uint8_t* rgb, uint32_t size, void* user);

// A patcher stays registered for as long as it has something to write or something to put back.
void Sync() {
    for (int a = 1; a <= kAtlases; ++a) {
        const bool want = (a == 3 && g_clonesPatch && !g_reservations.empty()) || HasVanilla(a) || HasSnapshot(a);
        if (want && !g_handles[a]) {
            const std::string name = "Weapon Panel Icons" + std::to_string(a);
            g_handles[a] = upload::AddPatcher(name.c_str(), &OnUpload, reinterpret_cast<void*>(static_cast<intptr_t>(a)));
        } else if (!want && g_handles[a]) {
            upload::RemovePatcher(g_handles[a]);
            g_handles[a] = 0;
        }
    }
}

void OnUpload(const char*, uint16_t w, uint16_t h, uint32_t fmt, uint8_t* rgb, uint32_t size, void* user) {
    const int atlas = static_cast<int>(reinterpret_cast<intptr_t>(user));
    if (atlas < 1 || atlas > kAtlases) return;
    if (w != kAtlasDim || h != kAtlasDim || fmt != kAtlasFmt || size != static_cast<uint32_t>(kAtlasBytes)) {
        if (!g_warnedShape[atlas]) {
            LOG_ERROR("[assets] Weapon Panel Icons%d upload is %ux%u fmt %u size %u, not the expected 256x256/0/196608:"
                       " leaving panel icons unpatched",
                       atlas, w, h, fmt, size);
            g_warnedShape[atlas] = true;
        }
        return;
    }
    if (atlas == 3 && g_clonesPatch)
        for (auto& r : g_reservations) WriteSubIcon(rgb, size, r.sub, r.rgba.data());
    // The vanilla pixels first, before anything of ours is written to a sub-icon for the first time; then put every
    // kept-aside icon back (a no-op over a freshly built atlas, the undo over one that still holds an old patch);
    // then write the patches that are registered now.
    for (auto& v : g_vanilla) {
        if (v.atlas != atlas) continue;
        bool have = false;
        for (auto& s : g_snaps) have |= s.atlas == atlas && s.sub == v.sub;
        if (have) continue;
        Snapshot s{atlas, v.sub, std::vector<uint8_t>(kSubBytes)};
        if (ReadSubIcon(rgb, size, v.sub, s.rgb.data())) g_snaps.push_back(std::move(s));
    }
    for (auto& s : g_snaps)
        if (s.atlas == atlas) RestoreSubIcon(rgb, size, s.sub, s.rgb.data());
    for (auto& v : g_vanilla)
        if (v.atlas == atlas) WriteSubIcon(rgb, size, v.sub, v.rgba.data());
}
bool Decode(const std::wstring& assetsDir, const std::string& relPng, std::vector<uint8_t>* rgba, std::string* err) {
    if (!SafeRel(relPng)) {
        if (err) *err = "panelIcon must be a relative path with no '..'";
        return false;
    }
    const std::wstring path = assetsDir + L"\\" + ToBackslash(game::Widen(relPng));
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (err) *err = "could not open the PNG";
        return false;
    }
    // Read just the header first: a hostile PNG could advertise a huge size, and stb_image would allocate for it
    // before Downscale ever got a chance to refuse. stbi_info doesn't decode pixels.
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_file(f, &w, &h, &n) || w != h || w % kSize != 0 || w > 4096) {
        fclose(f);
        if (err) *err = "must be a square PNG whose side is a multiple of 64, up to 4096";
        return false;
    }
    rewind(f);
    uint8_t* px = stbi_load_from_file(f, &w, &h, &n, 4);
    fclose(f);
    if (!px) {
        if (err) *err = "could not decode the PNG";
        return false;
    }
    rgba->resize(kSize * kSize * 4);
    const bool ok = Downscale(px, w, h, rgba->data(), err);
    stbi_image_free(px);
    return ok;
}

const Decoded* Cached(const std::string& modId, const std::string& relPng) {
    for (auto& d : g_decoded)
        if (d.modId == modId && d.relPng == relPng) return &d;
    return nullptr;
}
}  // namespace

void Preload(const std::string& modId, const std::wstring& assetsDir, const std::string& relPng) {
    if (Cached(modId, relPng)) return;
    Decoded d{modId, relPng, {}, {}};
    if (!Decode(assetsDir, relPng, &d.rgba, &d.err)) d.rgba.clear();
    g_decoded.push_back(std::move(d));
}

void Activate(bool on) {
    g_clonesPatch = on;
    Sync();
}

bool PatchVanilla(const std::string& modId, const std::wstring& assetsDir, const std::string& relPng, uint32_t iconCode,
                  std::string* err) {
    const int atlas = static_cast<int>(iconCode & 0xff), sub = static_cast<int>((iconCode >> 8) & 0xff);
    if (atlas < 1 || atlas > kAtlases || sub >= kSubTotal || (iconCode >> 16) != 0) {
        if (err) *err = "the weapon's icon code names no sub-icon of the three panel atlases";
        return false;
    }
    if (atlas == 3 && sub >= kFirstSub) {
        if (err) *err = "that sub-icon belongs to the clones' panel icons";
        return false;
    }
    VanillaPatch p;
    p.modId = modId;
    p.relPng = relPng;
    p.atlas = atlas;
    p.sub = sub;
    if (const Decoded* d = Cached(modId, relPng)) {
        if (d->rgba.empty()) {
            if (err) *err = d->err;
            return false;
        }
        p.rgba = d->rgba;
    } else if (!Decode(assetsDir, relPng, &p.rgba, err)) {
        return false;
    }
    for (auto& v : g_vanilla)
        if (v.atlas == atlas && v.sub == sub) {
            v = std::move(p);
            Sync();
            return true;
        }
    g_vanilla.push_back(std::move(p));
    Sync();
    LOG_INFO("[assets] %s: vanilla panel icon atlas %d sub-icon %d replaced by '%s'", modId.c_str(), atlas, sub, relPng.c_str());
    return true;
}

void ClearVanilla() {
    g_vanilla.clear();
    Sync();  // the patchers with a snapshot stay, to put the vanilla pixels back when the atlas is next uploaded
}

uint32_t VanillaCount() { return static_cast<uint32_t>(g_vanilla.size()); }

bool Reserve(const std::string& modId, const std::wstring& assetsDir, const std::string& relPng, uint32_t* iconCode,
             std::string* err) {
    if (iconCode) *iconCode = 0;
    for (auto& r : g_reservations)
        if (r.modId == modId && r.relPng == relPng) {
            if (iconCode) *iconCode = 3u | static_cast<uint32_t>(r.sub << 8);
            return true;
        }
    if (static_cast<int>(g_reservations.size()) >= kSubCount) {
        if (err) *err = "no free panel icon slot (at most 3 clones, 7 slots)";
        return false;
    }
    Reservation r;
    r.modId = modId;
    r.relPng = relPng;
    if (const Decoded* d = Cached(modId, relPng)) {
        if (d->rgba.empty()) {
            if (err) *err = d->err;
            return false;
        }
        r.rgba = d->rgba;
    } else if (!Decode(assetsDir, relPng, &r.rgba, err)) {
        return false;
    }
    r.sub = kFirstSub + static_cast<int>(g_reservations.size());
    const int sub = r.sub;
    g_reservations.push_back(std::move(r));
    g_clonesPatch = true;
    Sync();
    if (iconCode) *iconCode = 3u | static_cast<uint32_t>(sub << 8);
    LOG_INFO("[assets] %s: panel icon '%s' reserved as sub-icon %d", modId.c_str(), relPng.c_str(), sub);
    return true;
}

uint32_t Count() { return static_cast<uint32_t>(g_reservations.size()); }
}  // namespace melange::assets::icons
