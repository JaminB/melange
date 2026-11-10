// render.camera, render.shot: a look at the running game for tools (erg-mcp's game_camera and game_shots) that needs no
// client mod. render.camera sets the render-only camera override (render::SetCameraOverride, the same one
// wum.render.cameraOverride sets) owned by Oasis, or clears it; render.shot reads the back buffer at the Frame event
// (after the game's own frame, its HUD and post-FX, before the screen stage and the overlay) into a PNG under
// Documents\Melange\shots. A clean shot first hides the mods' wum.draw primitives for a few frames (draw::HideUntil), so
// a mod's text is not in the picture; the call then answers not ready until a frame drawn without them is there to read.
// Both run on the main thread (the Frame event, where the GL context is current) and change only the picture.
//
// The turn's "Ready?" intro can draw its main view from another camera than the one the override writes (in the Caldera
// iteration-10 Test every castle shot taken in an intro showed the worm and the HUD, the override only in the corner
// inset), so render.camera {when: "play"} holds a pose until the turn is in play (wum.game.turn().phase) and sets it
// then, and render.shot {requirePlay, afterCamera} answers not ready until the turn plays and the override has been drawn
// that many frames. render.fireShot arms a pinned camera that is set when the level's mine factory fires and read back
// `delayMs` later (the mines in flight), `perFire` times `everyMs` apart, written as <name>_<n>.png. With `track` the
// camera follows the fire's first mine (levels::factory::LastFlight: its start, its upward speed and the gravity, so
// where it is at each shot), `track` world units from it on the pinned pose's own bearing and a third of that over it,
// so the mine fills the picture instead of a dot over the crater (the Caldera v2 round-2 flight shots).
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
#include "levels/factory.h"
#include "lua/sandbox_core.h"
#include "melange/render.h"
#include "melange/sim.h"
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
constexpr ULONGLONG kPendingMs = 120000;   // a pose waiting for the turn's play phase is dropped after this
constexpr double kMaxFireShots = 512;      // render.fireShot's max: a run of 8+ volleys at several shots each in one arming

// Main thread (the RPCs and the Frame event run there).
struct Pose {
    render::CameraOverride o{};
    int frames = 30;
};
bool g_pending = false;           // a render.camera {when: "play"} pose waiting for the turn to play
Pose g_pend;
ULONGLONG g_pendSince = 0;
uint64_t g_camFrame = 0;          // the frame the override was last set at (by render.camera or the fire shot)
struct FireShot {
    bool armed = false;
    Pose pose;
    std::string name;
    int delayMs = 500, max = 4, holdFrames = 2, taken = 0;
    int perFire = 1, everyMs = 250, ofFire = 0;   // shots per fire, apart, and taken of the fire being shot
    float track = 0;              // world units: follow the fire's first mine from this far (0: the pinned pose)
    uint32_t serial = 0;          // the fire serial it waits past
    uint32_t firing = 0;          // the fire being shot (0: none)
    uint64_t setFrame = 0;        // the frame its camera was set at
    std::vector<std::string> paths;
    std::string lastError;
} g_fs;

// The turn's phase as wum.game.turn() says it ("intro", "play", "retreat", "settle", "none"), and the Ready? intro.
const char* Phase() { return sandbox::Game().phase; }
bool InIntro() { return sandbox::Game().ui.readyIntro || std::strcmp(sandbox::Game().phase, "intro") == 0; }
bool Playing() { return sim::InMatch() && std::strcmp(sandbox::Game().phase, "play") == 0 && !sandbox::Game().ui.readyIntro; }
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
        g_pending = false;
        const bool cleared = render::ClearCameraOverride(&g_owner);
        r.json = jsonmini::Obj().Bool("cleared", cleared).UInt("frame", frame).End();
        return;
    }
    render::CameraOverride o{};
    o.up[1] = 1.f;
    double fov = 0, frames = 30;
    std::string when;
    if (!Vec(p, "pos", o.pos, true, r) || !Vec(p, "target", o.target, true, r) || !Vec(p, "up", o.up, false, r) ||
        !Num(p, "fov", 5, 150, &fov, r) || !Num(p, "frames", 1, 600, &frames, r) || !rpc::Str(p, "when", &when, r, false))
        return;
    if (!when.empty() && when != "now" && when != "play") {
        Fail(r, rpc::kBadParams, "when: now (default) or play");
        return;
    }
    o.fovDeg = static_cast<float>(fov);
    g_pending = false;
    if (when == "play" && !Playing()) {
        // the turn's intro (or the gap between turns): hold the pose until the turn plays (OnFrame sets it)
        if (!sim::InMatch()) {
            Fail(r, rpc::kNotInMatch, "camera overrides are unavailable (no match is drawing)");
            return;
        }
        g_pending = true;
        g_pend = Pose{o, static_cast<int>(frames)};
        g_pendSince = GetTickCount64();
        r.json = jsonmini::Obj().Bool("set", false).Bool("pending", true).Str("phase", Phase()).UInt("frame", frame).End();
        return;
    }
    switch (render::SetCameraOverride(&g_owner, o, static_cast<int>(frames))) {
        case render::OverrideResult::Ok:
            g_camFrame = frame;
            r.json = jsonmini::Obj().Bool("set", true).UInt("frame", frame).Int("frames", static_cast<int>(frames)).Str("phase", Phase()).End();
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
    double width = 0, minFrame = 0, afterCamera = -1;
    bool clean = false, requirePlay = false;
    if (!Num(p, "width", 64, 8192, &width, r) || !Num(p, "minFrame", 0, 1e15, &minFrame, r) || !Num(p, "afterCamera", 0, 600, &afterCamera, r)) return;
    if (const json::Value* v = p.Get("clean"); v && !v->IsNull() && !rpc::Flag(p, "clean", &clean, r)) return;
    if (const json::Value* v = p.Get("requirePlay"); v && !v->IsNull() && !rpc::Flag(p, "requirePlay", &requirePlay, r)) return;
    const uint64_t frame = events::FrameCount();
    // the turn's intro draws from another camera: wait for the turn to play, and for the override (set, or pending
    // until the turn plays) to have been drawn afterCamera frames
    if ((requirePlay && !Playing()) || (afterCamera >= 0 && (g_pending || !render::CameraOverridden()))) {
        r.json = jsonmini::Obj().Bool("ready", false).UInt("frame", frame).Str("phase", Phase()).Bool("readyIntro", InIntro())
                     .Bool("cameraPending", g_pending).Str("waiting", requirePlay && !Playing() ? "the turn to play" : "the camera override").End();
        return;
    }
    uint64_t ready = static_cast<uint64_t>(minFrame);
    if (afterCamera >= 0) ready = std::max(ready, g_camFrame + static_cast<uint64_t>(afterCamera));
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
                 .UInt("frame", frame).Bool("clean", draw::HiddenAt(frame)).Bool("cameraOverride", render::CameraOverridden())
                 .Str("phase", Phase()).Bool("readyIntro", InIntro()).End();
}

// ---- the Frame event: a pending pose set once the turn plays; the fire shot
bool WritePng(const std::string& name, std::string* path, std::string* why) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (!ReadBack(&w, &h, &rgb)) return *why = "no frame to read", false;
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), w, h, 3, &len, 6, MZ_FALSE);
    if (!png) return *why = "the PNG could not be encoded", false;
    const std::wstring dir = ShotDir(), wpath = dir + L"\\" + game::Widen(name) + L".png";
    std::error_code ec;
    fs::create_directories(dir, ec);
    bool ok = false;
    if (FILE* f = _wfopen(wpath.c_str(), L"wb")) {
        ok = fwrite(png, 1, len, f) == len;
        fclose(f);
    }
    mz_free(png);
    if (!ok) return *why = "could not write " + game::Narrow(wpath), false;
    *path = game::Narrow(wpath);
    return true;
}

void OnFrame() {
    const uint64_t frame = events::FrameCount();
    if (g_pending) {
        if (!sim::InMatch() || GetTickCount64() - g_pendSince > kPendingMs) g_pending = false;
        else if (Playing()) {
            g_pending = false;
            if (render::SetCameraOverride(&g_owner, g_pend.o, g_pend.frames) == render::OverrideResult::Ok) {
                g_camFrame = frame;
                LOG_INFO("[oasis] render.camera: the turn plays; the pending pose is set (frame %llu)", static_cast<unsigned long long>(frame));
            }
        }
    }
    FireShot& f = g_fs;
    if (!f.armed) return;
    if (!sim::InMatch()) {
        f.armed = false;
        f.firing = 0;
        return;
    }
    ULONGLONG at = 0;
    const uint32_t serial = levels::factory::FireSerial(&at);
    if (!f.firing && serial != f.serial) {
        f.serial = serial;
        f.firing = serial;
        f.setFrame = 0;
        f.ofFire = 0;
    }
    if (!f.firing || GetTickCount64() - at < static_cast<ULONGLONG>(f.delayMs + f.ofFire * f.everyMs)) return;
    if (!f.setFrame) {
        // the pose, held a few frames: the shot is read once it has been drawn holdFrames times; tracking, the pinned
        // pose's bearing moved onto where the fire's first mine is now
        render::CameraOverride o = f.pose.o;
        levels::factory::Flight fl;
        if (f.track > 0 && levels::factory::LastFlight(&fl) && fl.serial == f.firing) {
            // where the followed mine is: once Melange has landed it (or it went into the sea) its landing, else its launch
            // arc, and a mine that has come back down past where it started is taken to rest there
            const float tms = static_cast<float>(GetTickCount64() - fl.at), down = fl.vy > 0 ? 2.f * fl.vy / fl.gravity : 0.f;
            const float tt = fl.vy > 0 ? (std::min)(tms, down) : tms;
            float m[3] = {fl.from[0] + fl.vx * tt, fl.from[1] + fl.vy * tt - 0.5f * fl.gravity * tt * tt, fl.from[2] + fl.vz * tt};
            if (fl.landed) std::memcpy(m, fl.landedAt, sizeof m);
            float dx = o.target[0] - o.pos[0], dz = o.target[2] - o.pos[2];
            const float l = std::hypot(dx, dz);
            if (l > 1e-3f) dx /= l, dz /= l; else dx = 0, dz = -1;
            o.pos[0] = m[0] - dx * f.track, o.pos[1] = m[1] + f.track / 3.f, o.pos[2] = m[2] - dz * f.track;
            std::memcpy(o.target, m, sizeof m);
        }
        if (render::SetCameraOverride(&g_owner, o, f.holdFrames + 30) != render::OverrideResult::Ok) {
            f.lastError = "the camera override was refused (a mod's override is in effect?)";
            f.firing = 0;
            return;
        }
        f.setFrame = frame;
        g_camFrame = frame;
        return;
    }
    if (frame < f.setFrame + static_cast<uint64_t>(f.holdFrames)) return;
    std::string path, why;
    const std::string name = f.name + "_" + std::to_string(++f.taken);
    if (WritePng(name, &path, &why)) {
        f.paths.push_back(path);
        LOG_INFO("[oasis] render.fireShot: %s, %d ms after the factory's fire (frame %llu)", path.c_str(), f.delayMs, static_cast<unsigned long long>(frame));
    } else f.lastError = why;
    f.setFrame = 0;
    if (++f.ofFire >= f.perFire || f.taken >= f.max) {
        render::ClearCameraOverride(&g_owner);
        f.firing = 0;
    }
    if (f.taken >= f.max) {
        render::ClearCameraOverride(&g_owner);
        f.armed = false;
    }
}

// render.fireShot {pos, target, fov?, name, delayMs?, max?, holdFrames?, perFire?, everyMs?, track?} | {clear: true} | {} (the state)
void FireShotRpc(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    FireShot& f = g_fs;
    auto state = [&] {
        jsonmini::Arr paths;
        for (const std::string& q : f.paths) paths.Str(q);
        jsonmini::Obj o;
        o.Bool("armed", f.armed).Str("name", f.name).Int("delayMs", f.delayMs).Int("perFire", f.perFire).Int("everyMs", f.everyMs)
            .Float("track", f.track).Int("taken", f.taken).Int("max", f.max).Raw("paths", paths.End())
            .UInt("fires", levels::factory::FireSerial(nullptr));
        if (!f.lastError.empty()) o.Str("lastError", f.lastError);
        return o.End();
    };
    if (const json::Value* cl = p.Get("clear"); cl && cl->IsBool() && cl->boolean) {
        f.armed = false;
        f.firing = 0;
        r.json = state();
        return;
    }
    if (!p.Get("pos")) {
        r.json = state();
        return;
    }
    Pose pose;
    pose.o.up[1] = 1.f;
    std::string name;
    double fov = 0, delay = 500, max = 4, hold = 2, per = 1, every = 250, track = 0;
    if (!Vec(p, "pos", pose.o.pos, true, r) || !Vec(p, "target", pose.o.target, true, r) || !Num(p, "fov", 5, 150, &fov, r) ||
        !rpc::Str(p, "name", &name, r) || !Num(p, "delayMs", 0, 10000, &delay, r) || !Num(p, "max", 1, kMaxFireShots, &max, r) || !Num(p, "holdFrames", 1, 60, &hold, r) ||
        !Num(p, "perFire", 1, 12, &per, r) || !Num(p, "everyMs", 50, 5000, &every, r) || !Num(p, "track", 0, 4000, &track, r))
        return;
    if (!ValidName(name) || name.size() > 56) {
        Fail(r, rpc::kBadParams, "name: 1-56 of A-Z a-z 0-9 _ - . (the shots are <name>_<n>.png)");
        return;
    }
    if (!sim::InMatch()) {
        Fail(r, rpc::kNotInMatch, "no match is running");
        return;
    }
    pose.o.fovDeg = static_cast<float>(fov);
    f = FireShot{};
    f.armed = true;
    f.pose = pose;
    f.name = name;
    f.delayMs = static_cast<int>(delay);
    f.max = static_cast<int>(max);
    f.holdFrames = static_cast<int>(hold);
    f.perFire = static_cast<int>(per);
    f.everyMs = static_cast<int>(every);
    f.track = static_cast<float>(track);
    f.serial = levels::factory::FireSerial(nullptr);
    r.json = state();
}
}  // namespace

void InstallRenderShot() {
    AddMethod("render.camera", &Camera, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("render.shot", &Shot, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("render.fireShot", &FireShotRpc, nullptr, kRpcMutating | kRpcGameOnly);
    events::Subscribe(events::Event::Frame, [] { OnFrame(); });
}
}  // namespace melange::oasis::providers
