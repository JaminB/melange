// render.camera, render.shot: a look at the running game for tools (erg-mcp's game_camera and game_shots) that needs no
// client mod. render.camera sets the render-only camera override (render::SetCameraOverride, the same one
// wum.render.cameraOverride sets) owned by Oasis, or clears it; render.shot reads the back buffer at the Frame event
// (after the game's own frame, its HUD and post-FX, before the screen stage and the overlay) into a PNG under
// Documents\Melange\shots. A clean shot first hides the mods' wum.draw primitives for a few frames (draw::HideUntil), so
// a mod's text is not in the picture; the call then answers not ready until a frame drawn without them is there to read.
// Both run on the main thread (the Frame event, where the GL context is current) and change only the picture.
#include <windows.h>
#include <shlobj.h>

#include <GL/gl.h>

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "melange/render.h"
#include "oasis/providers.h"
#include "oasis/rpc/params.h"
#include "render/mirage/draw_gl.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;
namespace fs = std::filesystem;

const char g_owner = 0;   // the override's owner: Oasis, not a mod
constexpr int kCleanFrames = 3;   // frames drawn without the mods' primitives before a clean shot is read
constexpr GLenum kReadFramebuffer = 0x8CA8, kReadFramebufferBinding = 0x8CAA, kPixelPackBuffer = 0x88EB,
                 kPixelPackBufferBinding = 0x88ED;

template <class T>
T Proc(const char* n) {
    PROC f = wglGetProcAddress(n);
    const auto v = reinterpret_cast<intptr_t>(f);
    return (f && v != 1 && v != 2 && v != 3 && v != -1) ? reinterpret_cast<T>(f) : nullptr;
}

bool Vec(const json::Value& p, const char* key, float out[3], bool required, Result& r) {
    const json::Value* v = p.Get(key);
    if (!v || v->IsNull()) return required ? Fail(r, rpc::kBadParams, std::string(key) + " is required") : true;
    if (!v->IsArray() || v->items.size() != 3) return Fail(r, rpc::kBadParams, std::string(key) + " must be [x, y, z]");
    for (int i = 0; i < 3; ++i) {
        if (!v->items[i].IsNumber() || !std::isfinite(v->items[i].number) || std::fabs(v->items[i].number) > 1e6)
            return Fail(r, rpc::kBadParams, std::string(key) + " must be three finite numbers");
        out[i] = static_cast<float>(v->items[i].number);
    }
    return true;
}

bool Num(const json::Value& p, const char* key, double lo, double hi, double* out, Result& r) {
    const json::Value* v = p.Get(key);
    if (!v || v->IsNull()) return true;
    if (!v->IsNumber() || v->number < lo || v->number > hi)
        return Fail(r, rpc::kBadParams, std::string(key) + " must be a number from " + std::to_string(static_cast<int>(lo)) + " to " +
                                            std::to_string(static_cast<int>(hi)));
    *out = v->number;
    return true;
}

// render.camera {pos, target, up?, fov?, frames?} | {clear: true}
void Camera(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    const uint64_t frame = events::FrameCount();
    if (const json::Value* cl = p.Get("clear"); cl && cl->IsBool() && cl->boolean) {
        const bool cleared = render::ClearCameraOverride(&g_owner);
        r.json = jsonmini::Obj().Bool("cleared", cleared).UInt("frame", frame).End();
        return;
    }
    render::CameraOverride o{};
    o.up[1] = 1.f;
    double fov = 0, frames = 30;
    if (!Vec(p, "pos", o.pos, true, r) || !Vec(p, "target", o.target, true, r) || !Vec(p, "up", o.up, false, r) ||
        !Num(p, "fov", 5, 150, &fov, r) || !Num(p, "frames", 1, 600, &frames, r))
        return;
    o.fovDeg = static_cast<float>(fov);
    switch (render::SetCameraOverride(&g_owner, o, static_cast<int>(frames))) {
        case render::OverrideResult::Ok:
            r.json = jsonmini::Obj().Bool("set", true).UInt("frame", frame).Int("frames", static_cast<int>(frames)).End();
            return;
        case render::OverrideResult::Busy:
            Fail(r, rpc::kBusy, "a mod's camera override is in effect (wum.render.cameraOverride); it ends by itself, or clear it in that mod");
            return;
        case render::OverrideResult::Invalid:
            Fail(r, rpc::kBadParams, "pos and target must differ, and up must not be along the view");
            return;
        default:
            Fail(r, rpc::kNotInMatch, "camera overrides are unavailable (no match is drawing, or this game build is not supported)");
            return;
    }
}

std::wstring ShotDir() {
    PWSTR p = nullptr;
    std::wstring docs = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) docs = p;
    if (p) CoTaskMemFree(p);
    return docs + L"\\Melange\\shots";
}

bool ValidName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    return std::all_of(n.begin(), n.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.'; }) &&
           n.find("..") == std::string::npos && n.front() != '.';
}

// The back buffer, top-down RGB, the read state put back as it was.
bool ReadBack(int* w, int* h, std::vector<uint8_t>* rgb) {
    if (!wglGetCurrentContext()) return false;
    render::WindowSize(w, h);
    if (*w <= 0 || *h <= 0 || *w > 8192 || *h > 8192) return false;
    auto bindFb = Proc<void(APIENTRY*)(GLenum, GLuint)>("glBindFramebuffer");
    auto bindBuf = Proc<void(APIENTRY*)(GLenum, GLuint)>("glBindBuffer");
    GLint readFbo = 0, packBuf = 0, readBuf = 0, align = 0, rowLen = 0, skipR = 0, skipP = 0;
    glGetIntegerv(kReadFramebufferBinding, &readFbo);
    glGetIntegerv(kPixelPackBufferBinding, &packBuf);
    glGetIntegerv(GL_READ_BUFFER, &readBuf);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLen);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skipR);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipP);
    if (readFbo && bindFb) bindFb(kReadFramebuffer, 0);
    if (packBuf && bindBuf) bindBuf(kPixelPackBuffer, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    std::vector<uint8_t> raw(static_cast<size_t>(*w) * *h * 3);
    glReadPixels(0, 0, *w, *h, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glPixelStorei(GL_PACK_ROW_LENGTH, rowLen);
    glPixelStorei(GL_PACK_SKIP_ROWS, skipR);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skipP);
    glReadBuffer(static_cast<GLenum>(readBuf));
    if (packBuf && bindBuf) bindBuf(kPixelPackBuffer, static_cast<GLuint>(packBuf));
    if (readFbo && bindFb) bindFb(kReadFramebuffer, static_cast<GLuint>(readFbo));
    rgb->resize(raw.size());
    const size_t row = static_cast<size_t>(*w) * 3;
    for (int y = 0; y < *h; ++y) std::memcpy(rgb->data() + y * row, raw.data() + (*h - 1 - y) * row, row);
    return true;
}

// A box-filtered copy at most `width` wide.
void Shrink(int w, int h, const std::vector<uint8_t>& in, int width, int* ow, int* oh, std::vector<uint8_t>* out) {
    *ow = std::max(1, std::min(w, width));
    *oh = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * *ow / w)));
    out->assign(static_cast<size_t>(*ow) * *oh * 3, 0);
    for (int y = 0; y < *oh; ++y) {
        const int y0 = y * h / *oh, y1 = std::max(y0 + 1, (y + 1) * h / *oh);
        for (int x = 0; x < *ow; ++x) {
            const int x0 = x * w / *ow, x1 = std::max(x0 + 1, (x + 1) * w / *ow);
            unsigned sum[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* q = in.data() + (static_cast<size_t>(yy) * w + xx) * 3;
                    sum[0] += q[0], sum[1] += q[1], sum[2] += q[2], ++n;
                }
            uint8_t* o = out->data() + (static_cast<size_t>(y) * *ow + x) * 3;
            for (int k = 0; k < 3; ++k) o[k] = static_cast<uint8_t>(sum[k] / n);
        }
    }
}

// render.shot {name, width?, clean?, minFrame?}
void Shot(const Call& c, Result& r, void*) {
    json::Value p;
    std::string name;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "name", &name, r)) return;
    if (!ValidName(name)) {
        Fail(r, rpc::kBadParams, "name: 1-64 of A-Z a-z 0-9 _ - . (a file name without .png)");
        return;
    }
    double width = 0, minFrame = 0;
    bool clean = false;
    if (!Num(p, "width", 64, 8192, &width, r) || !Num(p, "minFrame", 0, 1e15, &minFrame, r)) return;
    if (const json::Value* v = p.Get("clean"); v && !v->IsNull() && !rpc::Flag(p, "clean", &clean, r)) return;
    const uint64_t frame = events::FrameCount();
    uint64_t ready = static_cast<uint64_t>(minFrame);
    if (clean) {
        // the mods' primitives are hidden through this frame and the next kCleanFrames: read once two are drawn clean
        if (!draw::HiddenAt(frame)) {
            draw::HideUntil(frame + kCleanFrames + 2);
            ready = std::max(ready, frame + 2);
        }
    }
    if (frame < ready) {
        r.json = jsonmini::Obj().Bool("ready", false).UInt("frame", frame).UInt("readyAt", ready).End();
        return;
    }
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (!ReadBack(&w, &h, &rgb)) {
        Fail(r, rpc::kRefused, "no frame to read (no GL context on this thread, or the window has no size)");
        return;
    }
    int ow = w, oh = h;
    std::vector<uint8_t> shrunk;
    if (width > 0 && width < w) Shrink(w, h, rgb, static_cast<int>(width), &ow, &oh, &shrunk);
    const std::vector<uint8_t>& px = shrunk.empty() ? rgb : shrunk;
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(px.data(), ow, oh, 3, &len, 6, MZ_FALSE);
    if (!png) {
        Fail(r, rpc::kRefused, "the PNG could not be encoded");
        return;
    }
    const std::wstring dir = ShotDir(), path = dir + L"\\" + game::Widen(name) + L".png";
    std::error_code ec;
    fs::create_directories(dir, ec);
    bool ok = false;
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        ok = fwrite(png, 1, len, f) == len;
        fclose(f);
    }
    mz_free(png);
    if (!ok) {
        Fail(r, rpc::kRefused, "could not write " + game::Narrow(path));
        return;
    }
    LOG_INFO("[oasis] render.shot %s %dx%d frame %llu%s", name.c_str(), ow, oh, static_cast<unsigned long long>(frame), clean ? " (clean)" : "");
    r.json = jsonmini::Obj().Bool("ready", true).Str("path", game::Narrow(path)).Int("width", ow).Int("height", oh).UInt("bytes", len)
                 .UInt("frame", frame).Bool("clean", draw::HiddenAt(frame)).Bool("cameraOverride", render::CameraOverridden()).End();
}
}  // namespace

void InstallRenderShot() {
    AddMethod("render.camera", &Camera, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("render.shot", &Shot, nullptr, kRpcMutating | kRpcGameOnly);
}
}  // namespace melange::oasis::providers
