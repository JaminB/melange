#include "assets/icons.h"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "stb_image.h"

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
int g_patcherHandle = 0;
bool g_warnedShape = false;

void OnUpload(const char*, uint16_t w, uint16_t h, uint32_t fmt, uint8_t* rgb, uint32_t size, void*) {
    if (w != kAtlasDim || h != kAtlasDim || fmt != kAtlasFmt || size != static_cast<uint32_t>(kAtlasBytes)) {
        if (!g_warnedShape) {
            LOG_ERROR("[assets] Weapon Panel Icons3 upload is %ux%u fmt %u size %u, not the expected 256x256/0/196608:"
                       " leaving panel icons unpatched",
                       w, h, fmt, size);
            g_warnedShape = true;
        }
        return;
    }
    for (auto& r : g_reservations) WriteSubIcon(rgb, size, r.sub, r.rgba.data());
}
}  // namespace

bool Reserve(const std::string& modId, const std::wstring& assetsDir, const std::string& relPng, uint32_t* iconCode,
             std::string* err) {
    if (iconCode) *iconCode = 0;
    for (auto& r : g_reservations)
        if (r.modId == modId && r.relPng == relPng) {
            if (iconCode) *iconCode = 3u | static_cast<uint32_t>(r.sub << 8);
            return true;
        }
    if (!SafeRel(relPng)) {
        if (err) *err = "panelIcon must be a relative path with no '..'";
        return false;
    }
    if (static_cast<int>(g_reservations.size()) >= kSubCount) {
        if (err) *err = "no free panel icon slot (at most 3 clones, 7 slots)";
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
    Reservation r;
    r.modId = modId;
    r.relPng = relPng;
    r.rgba.resize(kSize * kSize * 4);
    const bool ok = Downscale(px, w, h, r.rgba.data(), err);
    stbi_image_free(px);
    if (!ok) return false;
    r.sub = kFirstSub + static_cast<int>(g_reservations.size());
    const int sub = r.sub;
    g_reservations.push_back(std::move(r));
    if (!g_patcherHandle) g_patcherHandle = upload::AddPatcher("Weapon Panel Icons3", &OnUpload, nullptr);
    if (iconCode) *iconCode = 3u | static_cast<uint32_t>(sub << 8);
    LOG_INFO("[assets] %s: panel icon '%s' reserved as sub-icon %d", modId.c_str(), relPng.c_str(), sub);
    return true;
}

uint32_t Count() { return static_cast<uint32_t>(g_reservations.size()); }
}  // namespace melange::assets::icons
