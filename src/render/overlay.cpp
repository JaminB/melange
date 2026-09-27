// Stub for component A (render/overlay core and input). Compiles and does nothing; A replaces this file.
// Public API: src/sdk/wumfix/overlay.h. See docs/m0-design.md §3 "A: render/overlay core and input".
//
// NOTE (added by the component D build, see its report): as shipped by E this file declared the header
// but defined none of it. D genuinely needs AddHotkey/AddMenuItem/ParseHotkey/Gl/Installed to link, and
// ParseHotkey in particular is plain string parsing that does not depend on any ImGui/DirectInput state,
// so it is implemented for real here (A's own ini defaults, ToggleKey/PassthroughKey, need exactly this).
// AddHotkey/AddMenuItem keep a real registry (so a handle is returned and can be looked up), but nothing
// ever calls the registered callbacks yet, because there is no Present hook or input capture installed -
// that is honestly reported through Installed() => false and Gl().valid => false, matching "no overlay
// running" rather than pretending one is. A's real implementation replaces this entire file.
#include "wumfix/overlay.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "core/keys.h"

namespace wf::overlay {
namespace {
struct HotkeyEntry {
    int handle;
    uint8_t dik, mods;
    ActionFn fn;
    void* user;
};
struct MenuEntry {
    int handle;
    std::string path;
    ActionFn fn;
    void* user;
};

std::mutex g_mx;
std::vector<HotkeyEntry> g_hotkeys;
std::vector<MenuEntry> g_menuItems;
int g_nextHandle = 1;
}  // namespace

int AddPanel(const char*, const char*, DrawFn, void*, uint32_t) { return 0; }
void RemovePanel(int) {}

int AddMenuItem(const char* path, ActionFn fn, void* user, const char*) {
    if (!path || !fn) return 0;
    std::lock_guard lk(g_mx);
    int h = g_nextHandle++;
    g_menuItems.push_back({h, path, fn, user});
    return h;
}

int AddHotkey(uint8_t dik, uint8_t mods, ActionFn fn, void* user) {
    if (!fn) return 0;
    std::lock_guard lk(g_mx);
    int h = g_nextHandle++;
    g_hotkeys.push_back({h, dik, mods, fn, user});
    return h;
}

// "Ctrl+Shift+F11" style text: '+'-separated modifier names (Ctrl/Shift/Alt, any case) followed by one
// key name from src/core/keys.h (case-insensitive). Returns false for an empty or unrecognised key name.
bool ParseHotkey(const char* text, uint8_t* dik, uint8_t* mods) {
    if (!text || !*text || !dik || !mods) return false;
    *dik = 0;
    *mods = kNone;
    std::string s(text);
    size_t pos = 0;
    std::string last;
    while (pos <= s.size()) {
        size_t plus = s.find('+', pos);
        std::string tok = s.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
        // trim
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        tok = (a == std::string::npos) ? std::string() : tok.substr(a, b - a + 1);
        if (plus == std::string::npos) {
            last = tok;
            break;
        }
        if (_stricmp(tok.c_str(), "Ctrl") == 0 || _stricmp(tok.c_str(), "Control") == 0)
            *mods |= kCtrl;
        else if (_stricmp(tok.c_str(), "Shift") == 0)
            *mods |= kShift;
        else if (_stricmp(tok.c_str(), "Alt") == 0)
            *mods |= kAlt;
        else if (!tok.empty())
            return false;  // unknown modifier token
        pos = plus + 1;
    }
    if (last.empty()) return false;
    const auto* k = wf::automation::FindKey(last.c_str());
    if (!k) return false;
    *dik = k->dik;
    return true;
}

bool Visible() { return false; }
void SetVisible(bool) {}
bool Capturing() { return false; }
void SetCapture(bool) {}

GlInfo Gl() { return {}; }  // valid=false: no Present hook has run yet in this build
Stats GetStats() { return {}; }
bool Installed() { return false; }
}  // namespace wf::overlay
