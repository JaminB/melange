#pragma once
// Pure window math of the Display module (borderless fullscreen): the styles, which rect to cover and where the
// window goes back to. No game, log or GL dependency, so it is self-tested offline (tests/display_selftest.cpp).
// Every rect is in the game's own coordinate space: WormsMayhem.exe is DPI-unaware, so on a scaled display Windows
// hands it virtualized (logical) monitor and window rects, and the math below never mixes them with physical ones.
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace melange::display::logic {
constexpr LONG kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER |
                              WS_DLGFRAME | WS_MAXIMIZE | WS_MINIMIZE;
constexpr LONG kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

// The game's windowed style without its frame, as a popup: WS_VISIBLE and the clip bits are kept.
inline LONG BorderlessStyle(LONG style) { return (style & ~kFrameStyles) | WS_POPUP; }
inline LONG BorderlessExStyle(LONG ex) { return ex & ~kFrameExStyles; }

inline int Width(const RECT& r) { return r.right - r.left; }
inline int Height(const RECT& r) { return r.bottom - r.top; }
inline bool Empty(const RECT& r) { return Width(r) <= 0 || Height(r) <= 0; }
inline bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
// `outer` contains all of `inner`.
inline bool Covers(const RECT& outer, const RECT& inner) {
    return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right && outer.bottom >= inner.bottom;
}
inline RECT Intersect(const RECT& a, const RECT& b) {
    RECT r{a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top, a.right < b.right ? a.right : b.right,
           a.bottom < b.bottom ? a.bottom : b.bottom};
    if (Empty(r)) r = RECT{0, 0, 0, 0};
    return r;
}

// A window without a caption that already covers its monitor: the engine's own exclusive fullscreen (/FS in
// local.cfg), which changed the display mode itself. Melange's borderless mode stays out of its way.
inline bool LooksExclusive(LONG style, const RECT& window, const RECT& monitor) {
    return !(style & WS_CAPTION) && !Empty(monitor) && Covers(window, monitor);
}

// Where the windowed game goes back to: its saved rect when enough of it (a 128x48 corner, so the title bar can be
// grabbed) is still on some monitor's work area, else the same size centred on `fallbackWork` (the work area of the
// monitor the window is on now), kept inside it when it is smaller. Monitors may sit at negative coordinates.
inline RECT RestoreRect(const RECT& saved, const std::vector<RECT>& workAreas, const RECT& fallbackWork) {
    for (const RECT& w : workAreas) {
        const RECT v = Intersect(saved, w);
        if (Width(v) >= 128 && Height(v) >= 48) return saved;
        if (Width(v) == Width(saved) && Height(v) == Height(saved) && !Empty(v)) return saved;  // a tiny window
    }
    if (Empty(fallbackWork)) return saved;
    const int w = Width(saved), h = Height(saved);
    LONG x = fallbackWork.left + (Width(fallbackWork) - w) / 2, y = fallbackWork.top + (Height(fallbackWork) - h) / 2;
    if (x < fallbackWork.left) x = fallbackWork.left;
    if (y < fallbackWork.top) y = fallbackWork.top;
    return RECT{x, y, x + w, y + h};
}

// The engine's window viewport (PCPostProcess caches it from GL_VIEWPORT when it builds its scene targets, and
// Composite restores it every frame) matches the client area, so the game draws to the whole window.
inline bool ViewportMatches(const int viewport[4], int clientW, int clientH) {
    return viewport[0] == 0 && viewport[1] == 0 && viewport[2] == clientW && viewport[3] == clientH;
}

// [Display] Hotkey: empty, "none" or "off" registers no hotkey.
inline bool HotkeyDisabled(const std::string& text) {
    size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
    if (a == std::string::npos) return true;
    const std::string t = text.substr(a, b - a + 1);
    return _stricmp(t.c_str(), "none") == 0 || _stricmp(t.c_str(), "off") == 0;
}
}  // namespace melange::display::logic
