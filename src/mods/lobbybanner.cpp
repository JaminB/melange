#include "mods/lobbybanner.h"

#include <algorithm>
#include <mutex>

#include "core/game.h"
#include "melange/draw.h"
#include "melange/render.h"
#include "net/net.h"

namespace melange::mods::lobbybanner {
namespace {
struct Slot {
    int handle, order;
    std::string owner, title;
    uint32_t color = 0xffffffffu;
    std::vector<std::string> lines;
};
std::mutex g_mx;
std::vector<Slot> g_slots;
int g_next = 1;
bool g_drawing = false;

constexpr float kTitlePx = 16.f, kTextPx = 15.f, kCharPx = 0.55f, kPad = 8.f, kGap = 6.f, kMargin = 16.f;
constexpr draw::Rgba kPanel = 0xd0180c08u, kText = 0xffe0e0e0u;

bool InLobbyScreen() { return game::IsKnownBuild() && wum::CurrentState() == wum::state::WaitingGameStart; }

void Draw(render::Stage, void*) {
    std::vector<Slot> slots;
    {
        std::lock_guard lk(g_mx);
        for (auto& s : g_slots)
            if (!s.title.empty()) slots.push_back(s);
    }
    if (slots.empty() || !InLobbyScreen()) return;
    int w = 0, h = 0;
    render::WindowSize(&w, &h);
    if (w <= 0 || h <= 0) return;
    const float x0 = std::max(kMargin, static_cast<float>(w) * 0.5f), x1 = static_cast<float>(w) - kMargin;
    const size_t chars = static_cast<size_t>(std::max(20.f, (x1 - x0 - 2 * kPad) / (kTextPx * kCharPx)));
    float y = 12.f;
    for (const auto& s : slots) {
        std::vector<std::string> title = Wrap(s.title, static_cast<size_t>(chars * kTextPx / kTitlePx));
        std::vector<std::string> body;
        for (const auto& l : s.lines)
            for (auto& part : Wrap(l, chars - 2)) body.push_back(std::move(part));
        const float height = 2 * kPad + static_cast<float>(title.size()) * (kTitlePx + 5.f) +
                             static_cast<float>(body.size()) * (kTextPx + 3.f);
        if (y + height > static_cast<float>(h) - kMargin) break;
        draw::HudRect(x0, y, x1, y + height, kPanel, true);
        draw::HudRect(x0, y, x1, y + height, s.color, false, 1.5f);
        float ty = y + kPad;
        for (const auto& t : title) {
            draw::HudText(x0 + kPad, ty, t.c_str(), s.color, kTitlePx);
            ty += kTitlePx + 5.f;
        }
        for (const auto& b : body) {
            draw::HudText(x0 + kPad + 12.f, ty, b.c_str(), kText, kTextPx);
            ty += kTextPx + 3.f;
        }
        y += height + kGap;
    }
}
}  // namespace

std::vector<std::string> Wrap(std::string_view text, size_t chars) {
    std::vector<std::string> out;
    if (chars < 8) chars = 8;
    size_t i = 0;
    while (i < text.size()) {
        size_t n = std::min(chars, text.size() - i);
        if (i + n < text.size()) {
            const size_t sp = text.rfind(' ', i + n);
            if (sp != std::string_view::npos && sp > i) n = sp - i;
        }
        out.emplace_back(text.substr(i, n));
        i += n;
        while (i < text.size() && text[i] == ' ') ++i;
    }
    return out;
}

int Add(const char* owner, int order) {
    std::lock_guard lk(g_mx);
    if (!g_drawing) {
        g_drawing = draw::AddDrawCallback(render::Stage::Hud, &Draw, nullptr) != 0;
    }
    Slot s;
    s.handle = g_next++;
    s.order = order;
    s.owner = owner ? owner : "";
    const int handle = s.handle;
    g_slots.push_back(std::move(s));
    std::stable_sort(g_slots.begin(), g_slots.end(), [](const Slot& a, const Slot& b) { return a.order < b.order; });
    return handle;
}

void Set(int slot, uint32_t color, std::string_view title, const std::vector<std::string>& lines) {
    std::lock_guard lk(g_mx);
    for (auto& s : g_slots)
        if (s.handle == slot) {
            s.color = color;
            s.title.assign(title);
            s.lines = title.empty() ? std::vector<std::string>{} : lines;
        }
}

void Remove(int slot) {
    std::lock_guard lk(g_mx);
    std::erase_if(g_slots, [slot](const Slot& s) { return s.handle == slot; });
}
}  // namespace melange::mods::lobbybanner
