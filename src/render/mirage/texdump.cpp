// Texture dumper: level 0 of every engine texture upload, decoded from the client memory the engine hands to
// glTexImage2D (not read back), written as PNG and deduplicated by content hash.
#include <windows.h>
#include <GL/gl.h>
#include <safetyhook.hpp>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "melange/compat.h"
#include "melange/gltrace.h"
#include "melange/jlog.h"
#include "render/mirage/engine.h"
#include "render/mirage/hub.h"
#include "render/mirage/trace_internal.h"
#include "tools/hash.h"

namespace melange::mirage::trace {
namespace {
// Engine texture upload (build #1077): cdecl (XImage*, char is3D, GLenum target). XImage: +0x14 char* name,
// +0x20 format id, +0x2c/+0x2e u16 width/height. Its glTexImage2D calls return into [kUpload, kUploadEnd).
constexpr uintptr_t kUpload = 0x79dc50, kUploadEnd = 0x79df90;

std::mutex g_mx;
std::atomic<uint32_t> g_budget{0}, g_dumped{0};
std::string g_filter;
std::set<std::string> g_seen;
std::map<uint32_t, std::string> g_names;
std::atomic<bool> g_wanted{false};
bool g_installed = false, g_installFailed = false;
SafetyHookMid g_uploadHook;
volatile uintptr_t g_curImage = 0;
std::wstring g_dir;

std::string ImageName(uintptr_t img) {
    uintptr_t p = 0;
    char s[128] = {};
    if (!img || !mem::SafeRead(img + 0x14, &p, 4) || p < 0x10000) return {};
    for (int i = 0; i < 127; ++i)
        if (!mem::SafeRead(p + i, &s[i], 1) || !s[i]) break;
    return s;
}

void OnTexImage2D(int, const uint32_t* f, void*) {
    // f[1..9]: target, level, internalformat, width, height, border, format, type, pixels
    uintptr_t caller = f[0];
    if (f[1] != GL_TEXTURE_2D || f[2] != 0 || caller < kUpload || caller >= kUploadEnd) return;
    uintptr_t img = g_curImage;
    std::string name = ImageName(img);
    if (!name.empty()) {
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        std::lock_guard lk(g_mx);
        if (g_names.size() < 65536) g_names[static_cast<uint32_t>(bound)] = name;
    }
    if (!g_budget.load(std::memory_order_relaxed) || !f[9]) return;
    uint32_t fmt = 0;
    if (img) mem::SafeRead(img + 0x20, &fmt, 4);
    std::string filter;
    {
        std::lock_guard lk(g_mx);
        filter = g_filter;
    }
    if (!filter.empty() && name.find(filter) == std::string::npos) return;

    int w = static_cast<int32_t>(f[4]), h = static_cast<int32_t>(f[5]);
    GLint align = 4, rowLen = 0, skipRows = 0, skipPix = 0, unpackBuf = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLen);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPix);
    glGetIntegerv(0x88EF /*GL_PIXEL_UNPACK_BUFFER_BINDING*/, &unpackBuf);
    if (unpackBuf || skipRows || skipPix || w <= 0 || h <= 0) return;
    uint64_t tight = PixelBytes(f[7], f[8], w, h, 1);
    if (!tight || tight > (64u << 20)) return;
    uint64_t bpp = tight / (static_cast<uint64_t>(w) * h), row = bpp * (rowLen > 0 ? rowLen : w);
    if (align > 1) row = (row + align - 1) / align * align;
    size_t bytes = static_cast<size_t>(row * (h - 1) + bpp * w);
    std::vector<uint8_t> raw(bytes);
    if (!mem::SafeRead(f[9], raw.data(), bytes)) return;
    std::vector<uint8_t> px;
    int ch = 0;
    if (!DecodeUpload(f[7], f[8], w, h, align, rowLen, raw.data(), &px, &ch)) {
        LOG_INFO("[mirage] texdump: %s %dx%d format 0x%x type 0x%x not decoded", name.c_str(), w, h, f[7], f[8]);
        return;
    }
    std::string hash = hashutil::Sha256Hex(px.data(), px.size());
    {
        std::lock_guard lk(g_mx);
        if (!g_seen.insert(hash).second) return;
    }
    uint32_t left = g_budget.load();
    while (left && !g_budget.compare_exchange_weak(left, left - 1)) {
    }
    if (!left) return;
    char file[200];
    snprintf(file, sizeof file, "%s_%dx%d_%u_%s.png", name.empty() ? "tex" : SafeFileName(name).c_str(), w, h,
             img ? fmt : f[7], hash.substr(0, 8).c_str());
    std::wstring path = g_dir + L"\\" + std::wstring(file, file + strlen(file));
    std::string logName = file;
    RunAsync([px = std::move(px), w, h, ch, path, logName] {
        std::string png = Png8(px.data(), w, h, ch);
        bool ok = !png.empty() && WriteFileBytes(path, png);
        LOG_INFO("[mirage] texdump %s %s", logName.c_str(), ok ? "written" : "FAILED");
    });
    ++g_dumped;
    jlog::Rec("mirage", jlog::Level::Info, "texdump").Str("file", logName).Str("name", name).Int("w", w).Int("h", h)
        .Uint("format", img ? fmt : f[7]).Str("sha256", hash);
    if (left == 1) LOG_INFO("[mirage] texdump: budget used up (%u dumped)", g_dumped.load());
}

bool InstallHooks() {
    if (g_installed || g_installFailed) return g_installed;
    if (!hub::Installed() || !game::IsKnownBuild() || !engine::Check() || !mem::Expect(kUpload, {0x83, 0xEC, 0x28, 0x55, 0x56})) {
        g_installFailed = true;
        LOG_WARN("[mirage] texdump unavailable: %s", hub::Installed() ? "engine upload function not recognised"
                                                                     : "needs the GL hub ([MirageTrace] Mode=count or log)");
        compat::Report(compat::Kind::Feature, "texdump", compat::Status::Skipped,
                       hub::Installed() ? "engine upload function not recognised" : "GL hub off", "builtin");
        return false;
    }
    g_uploadHook = safetyhook::create_mid(kUpload, [](safetyhook::Context& c) {
        uintptr_t img = 0;
        mem::SafeRead(c.esp + 4, &img, 4);
        g_curImage = img;
    });
    if (!g_uploadHook || AddTap("glTexImage2D", &OnTexImage2D, nullptr) < 0) {
        g_installFailed = true;
        LOG_WARN("[mirage] texdump: hook installation failed");
        return false;
    }
    g_installed = true;
    return true;
}
}  // namespace

std::wstring TexdumpDir() { return g_dir; }

std::string TextureName(uint32_t glName) {
    std::lock_guard lk(g_mx);
    auto it = g_names.find(glName);
    return it != g_names.end() ? it->second : std::string();
}

void TexdumpInstall(uint32_t atStart, const std::string& filter) {
    g_dir = MelangeDocsDir() + L"\\textures\\" + Widen(jlog::CurrentSession().id);
    if (atStart) gltrace::StartTextureDump(atStart, filter.empty() ? nullptr : filter.c_str());
    if (g_wanted) {
        g_wanted = false;
        InstallHooks();
    }
}

void TexdumpOnFrame() {
    if (g_wanted.exchange(false)) InstallHooks();
}
}  // namespace melange::mirage::trace

namespace melange::gltrace {
namespace t = mirage::trace;

bool StartTextureDump(uint32_t maxCount, const char* nameFilter) {
    if (!maxCount || !mirage::hub::Installed() || t::g_installFailed) return false;
    {
        std::lock_guard lk(t::g_mx);
        t::g_filter = nameFilter ? nameFilter : "";
    }
    t::EnsureDir(t::g_dir);
    t::g_budget = maxCount;
    if (!t::g_installed) t::g_wanted = true;
    LOG_INFO("[mirage] texdump: next %u engine uploads%s%s -> %s", maxCount, nameFilter ? " matching " : "",
             nameFilter ? nameFilter : "", game::Narrow(t::g_dir).c_str());
    return true;
}

void StopTextureDump() { t::g_budget = 0; }
uint32_t TexturesDumped() { return t::g_dumped; }
}  // namespace melange::gltrace
