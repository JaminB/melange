// SimAnnounce: wum.sim.announce(text[, seconds]) for sim mods and level scripts. A message drawn on this machine's HUD,
// centred near the top of the screen, for a few seconds: the rising sea, the next round, a goal reached. Every machine
// runs the sim, so every player sees it; it changes nothing the match reads (the sim, Wormsign hashes and replays are
// untouched), so it is free to call from deterministic code. Drawn at the Hud stage, after the engine's own HUD.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "core/module.h"
#include "lua/engine50.h"
#include "lua/sim/bridge_internal.h"
#include "melange/draw.h"
#include "melange/jlog.h"
#include "melange/render.h"

namespace {
namespace l5 = melange::lua50;

constexpr int kLines = 3;              // messages shown at once; a fourth replaces the oldest
constexpr size_t kMaxText = 160;       // bytes of UTF-8
constexpr float kMinSeconds = 0.5f, kMaxSeconds = 15.f, kDefaultSeconds = 4.f, kFadeMs = 400.f;

struct Line {
    char text[kMaxText + 1] = {};
    ULONGLONG shown = 0, until = 0;
};
std::mutex g_mx;
Line g_lines[kLines];
int g_stage = 0;
char g_err[96];

int Fail(l5::State* L, const char* msg) {
    snprintf(g_err, sizeof g_err, "%s", msg);
    l5::A().pushstring(L, g_err);
    l5::A().error(L);
    return 0;
}

// wum.sim.announce(text[, seconds]): true. Text is cut to 160 bytes (at a UTF-8 boundary); seconds 0.5 to 15, default 4.
int __cdecl LAnnounce(l5::State* L) {
    const auto& a = l5::A();
    if (a.type(L, 1) != l5::kTString && a.type(L, 1) != l5::kTNumber) return Fail(L, "wum.sim.announce: expected a text");
    float seconds = kDefaultSeconds;
    if (a.type(L, 2) == l5::kTNumber) seconds = a.tonumber(L, 2);
    else if (a.type(L, 2) != l5::kTNone && a.type(L, 2) != l5::kTNil) return Fail(L, "wum.sim.announce: seconds must be a number");
    if (!(seconds >= kMinSeconds)) seconds = kMinSeconds;
    if (seconds > kMaxSeconds) seconds = kMaxSeconds;
    const char* text = a.tostring(L, 1);
    size_t n = text ? std::strlen(text) : 0;
    if (n > kMaxText) {
        n = kMaxText;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;  // not inside a UTF-8 sequence
    }
    {
        std::lock_guard lk(g_mx);
        int slot = 0;
        for (int i = 1; i < kLines; ++i)
            if (g_lines[i].until < g_lines[slot].until) slot = i;
        Line& l = g_lines[slot];
        std::memcpy(l.text, text ? text : "", n);
        l.text[n] = 0;
        l.shown = GetTickCount64();
        l.until = l.shown + static_cast<ULONGLONG>(seconds * 1000.f);
    }
    const char* mod = melange::simbridge::CurrentMod();
    melange::jlog::Rec("sim", melange::jlog::Level::Info, "announce").Str("mod", mod ? mod : "?").Str("text", std::string_view(text ? text : "", n)).Float("seconds", seconds);
    a.pushboolean(L, 1);
    return 1;
}

void Draw(melange::render::Stage, void*) {
    const ULONGLONG now = GetTickCount64();
    Line show[kLines];
    int count = 0;
    {
        std::lock_guard lk(g_mx);
        for (const Line& l : g_lines)
            if (l.until > now) show[count++] = l;
    }
    if (!count) return;
    // oldest on top
    for (int i = 1; i < count; ++i)
        for (int j = i; j > 0 && show[j].shown < show[j - 1].shown; --j) std::swap(show[j], show[j - 1]);
    int w = 0, h = 0;
    melange::render::WindowSize(&w, &h);
    if (w <= 0 || h <= 0) return;
    const float size = h >= 1000 ? 40.f : h >= 700 ? 30.f : 22.f;
    melange::draw::TextOptions o;
    o.shadow = true;
    o.shadowDx = o.shadowDy = size / 12.f;
    o.align = melange::draw::Align::Center;
    o.maxWidth = static_cast<float>(w) * 0.8f;
    // under the engine's own banner (the team name and "Ready?" of the turn intro sit in the top fifth)
    float y = static_cast<float>(h) * 0.27f;
    for (int i = 0; i < count; ++i) {
        const float left = static_cast<float>(show[i].until - now), in = static_cast<float>(now - show[i].shown);
        float alpha = left < kFadeMs ? left / kFadeMs : 1.f;
        if (in < kFadeMs / 2) alpha = (std::min)(alpha, in / (kFadeMs / 2));
        const auto a8 = static_cast<uint32_t>(alpha * 255.f + 0.5f);
        const melange::draw::TextExtent e = melange::draw::MeasureText(show[i].text, size);
        const float half = (std::min)(e.w, o.maxWidth) / 2.f + size * 0.5f, cx = static_cast<float>(w) / 2.f;
        melange::draw::HudRect(cx - half, y - size * 0.2f, cx + half, y + e.h + size * 0.2f, (static_cast<uint32_t>(alpha * 150.f) << 24), true);
        o.shadowColor = (a8 << 24);
        melange::draw::HudTextEx(cx, y, show[i].text, (a8 << 24) | 0x0050D8FFu, size, o);
        y += e.h + size * 0.6f;
    }
}

class SimAnnounce final : public melange::Module {
public:
    const char* Name() const override { return "SimAnnounce"; }
    const char* Description() const override { return "on-screen messages from sim mods and level scripts (wum.sim.announce)"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 56; }  // after SimBridge (52)
    bool Install() override {
        if (!l5::Check()) {
            LOG_WARN("[sim] engine Lua check failed: SimAnnounce stays inert");
            return true;
        }
        if (!melange::simbridge::AddSimFunction("announce", &LAnnounce)) LOG_WARN("[sim] could not add wum.sim.announce");
        g_stage = melange::render::AddStageCallback(melange::render::Stage::Hud, &Draw, nullptr, 100);
        if (!g_stage) LOG_WARN("[sim] announce: no Hud stage callback; messages are not drawn");
        return true;
    }
    void Uninstall() override {
        if (g_stage) melange::render::RemoveStageCallback(g_stage);
        g_stage = 0;
    }
};
}  // namespace

MELANGE_MODULE(SimAnnounce);
