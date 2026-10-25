#pragma once
// Pure input logic of the overlay, free of game, log and ImGui dependencies so it can be self-tested.
#include <windows.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/keys.h"
#include "lua/event_names.h"
#include "render/input_regions.h"

namespace melange::render {
// Same values as melange::overlay::Mods.
enum : uint8_t { kModCtrl = 1, kModShift = 2, kModAlt = 4 };

constexpr uint8_t kDikLCtrl = 0x1D, kDikRCtrl = 0x9D, kDikLShift = 0x2A, kDikRShift = 0x36, kDikLAlt = 0x38,
                  kDikRAlt = 0xB8;

inline std::string TrimCopy(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "GRAVE", "Shift+GRAVE", "Ctrl+Shift+F11", "ctrl + o", "`", "0x29". The last token is the key (a keys.h name,
// "`" or a hex DIK code); every token before it must be Ctrl/Control, Shift or Alt. Case-insensitive.
inline bool ParseHotkeyText(const char* text, uint8_t* dik, uint8_t* mods) {
    if (!text || !dik || !mods) return false;
    std::vector<std::string> tok;
    std::string s(text);
    size_t p = 0;
    while (true) {
        size_t c = s.find('+', p);
        tok.push_back(TrimCopy(s.substr(p, c == std::string::npos ? std::string::npos : c - p)));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    if (tok.empty()) return false;
    uint8_t m = 0;
    for (size_t i = 0; i + 1 < tok.size(); ++i) {
        const char* t = tok[i].c_str();
        if (_stricmp(t, "ctrl") == 0 || _stricmp(t, "control") == 0)
            m |= kModCtrl;
        else if (_stricmp(t, "shift") == 0)
            m |= kModShift;
        else if (_stricmp(t, "alt") == 0)
            m |= kModAlt;
        else
            return false;
    }
    const std::string& key = tok.back();
    if (key.empty()) return false;
    uint8_t code = 0;
    if (key == "`") {
        code = 0x29;  // GRAVE
    } else if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        char* end = nullptr;
        unsigned long v = strtoul(key.c_str() + 2, &end, 16);
        if (!end || *end || v == 0 || v > 0xFF) return false;
        code = static_cast<uint8_t>(v);
    } else {
        const melange::automation::KeyDef* k = melange::automation::FindKey(key.c_str());
        if (!k) return false;
        code = k->dik;
    }
    *dik = code;
    *mods = m;
    return true;
}

// First keys.h name for a DIK code ("?" if none).
inline const char* DikName(uint8_t dik) {
    for (const auto& k : melange::automation::kKeys)
        if (k.dik == dik) return k.name;
    return "?";
}

inline std::string HotkeyLabel(uint8_t dik, uint8_t mods) {
    std::string s;
    if (mods & kModCtrl) s += "Ctrl+";
    if (mods & kModShift) s += "Shift+";
    if (mods & kModAlt) s += "Alt+";
    const char* n = DikName(dik);
    if (n[0] == '?') {
        char b[8];
        snprintf(b, sizeof(b), "0x%02X", dik);
        s += b;
    } else {
        s += n;
    }
    return s;
}

// Splits a menu path on '/', trimming blanks and dropping empty segments. False if nothing remains.
inline bool SplitMenuPath(const char* path, std::vector<std::string>& out) {
    out.clear();
    if (!path) return false;
    std::string s(path);
    size_t p = 0;
    while (p <= s.size()) {
        size_t c = s.find('/', p);
        std::string t = TrimCopy(s.substr(p, c == std::string::npos ? std::string::npos : c - p));
        if (!t.empty()) out.push_back(t);
        if (c == std::string::npos) break;
        p = c + 1;
    }
    return !out.empty();
}

// Splits a panel title into View-menu levels: "Thumper/Mods" -> {"Thumper", "Mods"}. Only a '/' with no blank on
// either side separates, so a title like "About / Stats" stays one item. Segments are trimmed and empty ones
// dropped; a title that leaves nothing stays whole.
inline std::vector<std::string> SplitPanelTitle(const std::string& title) {
    auto blank = [](char c) { return c == ' ' || c == '\t'; };
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= title.size(); ++i) {
        const bool cut = i == title.size() ||
                         (title[i] == '/' && i > 0 && i + 1 < title.size() && !blank(title[i - 1]) && !blank(title[i + 1]));
        if (!cut) continue;
        std::string t = TrimCopy(title.substr(start, i - start));
        if (!t.empty()) out.push_back(std::move(t));
        start = i + 1;
    }
    if (out.empty()) out.push_back(title);
    return out;
}

// One level of an overlay menu, built from item paths in registration order. `items` are the caller's indices of
// the items whose path ends at this node. A name that is both an item and a submenu ("A" and "A/B") becomes one
// submenu with the item first in it, so the two never share an ImGui id at one level.
struct MenuNode {
    std::string name;
    std::vector<int> items;
    std::vector<MenuNode> children;
};

inline void AddMenuNode(std::vector<MenuNode>& level, const std::vector<std::string>& segs, int item) {
    std::vector<MenuNode>* cur = &level;
    for (size_t i = 0; i < segs.size(); ++i) {
        MenuNode* node = nullptr;
        for (MenuNode& n : *cur)
            if (n.name == segs[i]) node = &n;
        if (!node) {
            cur->push_back(MenuNode{segs[i], {}, {}});
            node = &cur->back();
        }
        if (i + 1 == segs.size())
            node->items.push_back(item);
        else
            cur = &node->children;
    }
}

struct HotkeyDef {
    int handle;
    uint8_t dik;
    uint8_t mods;
};

// Window-message classification for the overlay's subclassed window procedure. Pure so it can be self-tested
// without a real window, DirectInput device or ImGui backend.
inline bool IsKeyMsg(UINT m) {
    return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR || m == WM_DEADCHAR || m == WM_SYSKEYDOWN ||
           m == WM_SYSKEYUP || m == WM_SYSCHAR;
}

inline bool IsButtonMsg(UINT m) {
    return (m >= WM_LBUTTONDOWN && m <= WM_MBUTTONDBLCLK) || (m >= WM_XBUTTONDOWN && m <= WM_XBUTTONDBLCLK);
}

// Messages ImGui's Win32 backend wants while the overlay is capturing.
inline bool ForImGui(UINT m) {
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_MOUSELEAVE || m == WM_KEYDOWN || m == WM_KEYUP ||
           m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SETFOCUS || m == WM_KILLFOCUS ||
           m == WM_INPUTLANGCHANGE;
}

inline bool SwallowForGame(UINT m) {
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT || m == WM_KEYDOWN || m == WM_KEYUP ||
           m == WM_CHAR;
}

// Whether a message reaching the subclassed window procedure must be kept from the game. Depends only on
// `capturing`, never on whether the ImGui backend happens to be ready for it: a message the overlay means to
// capture is dropped, not let through to the game, if ImGui cannot take it right now. That keeps a real WM_CHAR
// the only way text ever reaches an overlay box, and keeps a key the overlay is capturing (e.g. the one used to
// leave the attract demo) from ever reaching the game while a box still holds it.
inline bool CapturedFromGame(UINT m, bool capturing) { return capturing && SwallowForGame(m); }

// An Alt+key hotkey (Display's Alt+Enter) reaches the window as WM_SYSKEYDOWN and is dropped, so DefWindowProc never
// sees a key pressed with Alt and would take the Alt release for a lone Alt tap: the system-menu mode (SC_KEYMENU)
// of a window that has a system menu. Dropping that one Alt release keeps it out; a fresh Alt press forgets it.
class AltReleaseGuard {
public:
    // `droppedHotkey`: the window procedure is dropping this message as a hotkey's. True: drop this message too.
    bool Drop(UINT msg, WPARAM wp, LPARAM lp, bool droppedHotkey) {
        if (droppedHotkey) {
            if (msg == WM_SYSKEYDOWN) pending_ = true;
            return false;
        }
        if (wp != VK_MENU) return false;
        if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && !(lp & (1 << 30))) {
            pending_ = false;
            return false;
        }
        if ((msg == WM_SYSKEYUP || msg == WM_KEYUP) && pending_) {
            pending_ = false;
            return true;
        }
        return false;
    }
    bool Pending() const { return pending_; }

private:
    bool pending_ = false;
};

// Keyboard-buffer filter. Not thread-safe: the DirectInput poll runs on the game's main thread.
class KeyFilter {
public:
    // Filters `buf[0..n)` in place and returns the new count. Hotkey key-downs (and their key-ups) are removed
    // and reported in `fired`; while capturing everything is removed. When capture starts, synthetic key-ups are
    // appended for every key the game last saw down, so it doesn't see stuck keys.
    DWORD Process(DIDEVICEOBJECTDATA* buf, DWORD n, DWORD capacity, bool capturing, const HotkeyDef* hk, size_t nhk,
                  std::vector<int>* fired, std::vector<uint8_t>* released, DWORD nowTick) {
        if (capturing && !wasCapturing_) releasePending_ = true;
        wasCapturing_ = capturing;
        DWORD out = 0;
        for (DWORD i = 0; i < n; ++i) {
            const DIDEVICEOBJECTDATA d = buf[i];
            if (d.dwSequence > lastSeq_) lastSeq_ = d.dwSequence;
            if (d.dwOfs > 0xFF) {  // not a keyboard key
                if (!capturing && out < capacity) buf[out++] = d;
                continue;
            }
            const uint8_t dik = static_cast<uint8_t>(d.dwOfs);
            const bool down = (d.dwData & 0x80) != 0;
            if (down) {
                const uint8_t m = Mods();  // modifiers held before this key
                bool hit = false;
                for (size_t h = 0; h < nhk; ++h) {
                    if (hk[h].dik == dik && hk[h].mods == m) {
                        if (fired) fired->push_back(hk[h].handle);
                        hit = true;
                    }
                }
                phys_[dik] = true;
                if (hit) {
                    eaten_[dik] = true;
                    continue;
                }
            } else {
                phys_[dik] = false;
                if (eaten_[dik]) {
                    eaten_[dik] = false;
                    continue;
                }
            }
            if (capturing) {
                ++dropped_;
                continue;
            }
            game_[dik] = down;
            if (out < capacity) buf[out++] = d;
        }
        if (releasePending_) {
            bool left = false;
            for (int k = 0; k < 256; ++k) {
                if (!game_[k]) continue;
                if (out >= capacity) {
                    left = true;
                    break;
                }
                DIDEVICEOBJECTDATA r{};
                r.dwOfs = static_cast<DWORD>(k);
                r.dwData = 0;
                r.dwTimeStamp = nowTick;
                r.dwSequence = ++lastSeq_;
                buf[out++] = r;
                game_[k] = false;
                ++synthetic_;
                if (released) released->push_back(static_cast<uint8_t>(k));
            }
            releasePending_ = left;
        }
        return out;
    }

    // GetDeviceState(256-byte keyboard state): all zero while capturing, otherwise hotkey keys held down are hidden.
    void FilterState(uint8_t* keys, DWORD cb, bool capturing) const {
        if (!keys) return;
        if (capturing) {
            memset(keys, 0, cb);
            return;
        }
        for (DWORD i = 0; i < cb && i < 256; ++i)
            if (eaten_[i]) keys[i] = static_cast<uint8_t>(keys[i] & ~0x80);
    }

    uint8_t Mods() const {
        uint8_t m = 0;
        if (phys_[kDikLCtrl] || phys_[kDikRCtrl]) m |= kModCtrl;
        if (phys_[kDikLShift] || phys_[kDikRShift]) m |= kModShift;
        if (phys_[kDikLAlt] || phys_[kDikRAlt]) m |= kModAlt;
        return m;
    }

    // The device was re-created or lost focus: nothing is known to be held any more.
    void ResetDevice() {
        memset(phys_, 0, sizeof(phys_));
        memset(eaten_, 0, sizeof(eaten_));
        memset(game_, 0, sizeof(game_));
        releasePending_ = false;
    }

    // A record injected after Process: the game sees it, so a capture must release it like any other key.
    void NoteGame(uint8_t dik, bool down) { game_[dik] = down; }

    bool GameDown(uint8_t dik) const { return game_[dik]; }
    bool PhysDown(uint8_t dik) const { return phys_[dik]; }
    uint64_t Dropped() const { return dropped_; }
    uint64_t Synthetic() const { return synthetic_; }

private:
    bool phys_[256] = {};   // physically (or injected) down, from every record seen
    bool game_[256] = {};   // down as far as the game knows (records we passed through)
    bool eaten_[256] = {};  // key-down swallowed as a hotkey: swallow its key-up too
    bool wasCapturing_ = false, releasePending_ = false;
    DWORD lastSeq_ = 0;
    uint64_t dropped_ = 0, synthetic_ = 0;
};

// ---------------------------------------------------------------- mod input (Melange 0.12, wum.input)
// Bare F1..F12 hotkeys do not fire while a mod has keyboard focus (those keys go to its text box); hotkeys with a
// modifier, and the overlay's own bare keys, still do.
inline bool HotkeyLiveWhileFocused(uint8_t dik, uint8_t mods, bool focusHeld) {
    return mods != 0 || !melange::automation::IsFunctionKeyDik(dik) || !focusHeld;
}

// Mouse buttons in the order of the "button" field names.
enum : int { kBtnLeft, kBtnRight, kBtnMiddle, kBtnX1, kBtnX2, kBtnCount };

inline const char* ButtonName(int b) {
    static const char* const kNames[kBtnCount] = {"left", "right", "middle", "x1", "x2"};
    return b >= 0 && b < kBtnCount ? kNames[b] : "?";
}

struct ButtonMsg {
    int button = -1;  // -1: not a button message
    bool down = false, dbl = false;
};

inline ButtonMsg DecodeButton(UINT m, WPARAM wp) {
    ButtonMsg b;
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: b.button = kBtnLeft; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: b.button = kBtnRight; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: b.button = kBtnMiddle; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        b.button = HIWORD(wp) == XBUTTON2 ? kBtnX2 : kBtnX1;
        break;
    default: return b;
    }
    b.dbl = m == WM_LBUTTONDBLCLK || m == WM_RBUTTONDBLCLK || m == WM_MBUTTONDBLCLK || m == WM_XBUTTONDBLCLK;
    b.down = b.dbl || m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_XBUTTONDOWN;
    return b;
}

// The release message (and its wParam) for a button, for the synthetic releases sent when pointer mode starts.
inline UINT ButtonUpMsg(int b) {
    switch (b) {
    case kBtnLeft: return WM_LBUTTONUP;
    case kBtnRight: return WM_RBUTTONUP;
    case kBtnMiddle: return WM_MBUTTONUP;
    default: return WM_XBUTTONUP;
    }
}
inline WPARAM ButtonUpWParam(int b) {
    return b == kBtnX1 ? MAKEWPARAM(0, XBUTTON1) : b == kBtnX2 ? MAKEWPARAM(0, XBUTTON2) : 0;
}

// One owner-only melange.input.* event.
struct ModEvent {
    enum class Kind : uint8_t { Mouse, Wheel, Key, Char, Paste };
    Kind kind = Kind::Mouse;
    std::string mod;
    std::string region;  // mouse, wheel
    int button = kBtnLeft;
    bool down = false, dbl = false;
    float x = 0, y = 0, delta = 0;
    std::string key;     // key: the keys.h name
    bool repeat = false, ctrl = false, shift = false, alt = false;
    std::string text;    // char, paste (UTF-8)
};

inline const char* ModEventName(ModEvent::Kind k) {
    namespace ev = melange::sandbox::ev;
    switch (k) {
    case ModEvent::Kind::Mouse: return ev::kInputMouse;
    case ModEvent::Kind::Wheel: return ev::kInputWheel;
    case ModEvent::Kind::Key: return ev::kInputKey;
    case ModEvent::Kind::Char: return ev::kInputChar;
    default: return ev::kInputPaste;
    }
}

inline void AppendJsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

inline void AppendJsonNumber(std::string& out, double v) {
    if (!(v == v) || v > 1e9 || v < -1e9) v = 0;  // NaN or absurd: never invalid JSON
    char b[32];
    snprintf(b, sizeof(b), "%.6g", v);
    out += b;
}

inline std::string ModEventJson(const ModEvent& e) {
    std::string j = "{";
    auto key = [&](const char* k) {
        if (j.size() > 1) j += ',';
        j += '"';
        j += k;
        j += "\":";
    };
    auto boolean = [&](const char* k, bool v) {
        key(k);
        j += v ? "true" : "false";
    };
    switch (e.kind) {
    case ModEvent::Kind::Mouse:
        key("region");
        AppendJsonString(j, e.region);
        key("button");
        AppendJsonString(j, ButtonName(e.button));
        boolean("down", e.down);
        key("x");
        AppendJsonNumber(j, e.x);
        key("y");
        AppendJsonNumber(j, e.y);
        boolean("double", e.dbl);
        break;
    case ModEvent::Kind::Wheel:
        key("region");
        AppendJsonString(j, e.region);
        key("delta");
        AppendJsonNumber(j, e.delta);
        key("x");
        AppendJsonNumber(j, e.x);
        key("y");
        AppendJsonNumber(j, e.y);
        break;
    case ModEvent::Kind::Key:
        key("key");
        AppendJsonString(j, e.key);
        boolean("down", e.down);
        boolean("repeat", e.repeat);
        boolean("ctrl", e.ctrl);
        boolean("shift", e.shift);
        boolean("alt", e.alt);
        break;
    default:
        key("text");
        AppendJsonString(j, e.text);
        break;
    }
    j += '}';
    return j;
}

inline void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Text a mod may receive: no C0/C1 control characters, no DEL, no noncharacter U+FFFE/U+FFFF.
inline bool TextCodepoint(uint32_t cp) {
    return cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0) && cp != 0xFFFE && cp != 0xFFFF;
}

// melange.input.paste: clipboard UTF-16 -> UTF-8 with control characters removed (so a URL copied with its line
// break pastes as one line) and unpaired surrogates dropped, cut at a whole character to at most `maxBytes`.
inline std::string SanitizePaste(const std::wstring& w, size_t maxBytes = 4096) {
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = static_cast<uint16_t>(w[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= w.size()) break;
            const uint32_t lo = static_cast<uint16_t>(w[i + 1]);
            if (lo < 0xDC00 || lo > 0xDFFF) continue;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            ++i;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            continue;
        }
        if (!TextCodepoint(cp)) continue;
        std::string one;
        AppendUtf8(one, cp);
        if (out.size() + one.size() > maxBytes) break;
        out += one;
    }
    return out;
}

// The frontend cursor (MouseEntity +0x24/+0x28) to window pixels. The entity lives in a 640x480 space scaled by the
// game's aspect factors (0x4d4cc0: a = w/h of the render size, sx = a*0.75 and sy = 1 up to 16:9, sx = 4/3 beyond,
// sx = sy = a*0.75 below 4:3), centred on 0 with +y up (the cursor sprite hangs at +14,-14 from the hotspot,
// 0x744c0e), and 0x5073a0 divides by 320*sx, 240*sy for the menus' [-1, 1] mouse. The menus fill the window.
inline bool FeCursorToWindow(float fx, float fy, int aspW, int aspH, int winW, int winH, float* x, float* y) {
    if (winW <= 0 || winH <= 0 || !(fx == fx) || !(fy == fy)) return false;
    float a = aspW > 0 && aspH > 0 ? static_cast<float>(aspW) / static_cast<float>(aspH) : 4.0f / 3.0f;
    float sx = a * 0.75f, sy = 1.0f;
    if (a > 16.0f / 9.0f) {
        sx = 4.0f / 3.0f;
    } else if (a < 4.0f / 3.0f) {
        sy = a / (4.0f / 3.0f);
        sx = sy;
    }
    const float nx = fx / (320.0f * sx), ny = fy / (240.0f * sy);
    if (nx < -1.05f || nx > 1.05f || ny < -1.05f || ny > 1.05f) return false;  // not a clamped cursor position
    const auto clamp01 = [](float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
    *x = clamp01((nx + 1) * 0.5f) * static_cast<float>(winW);
    *y = clamp01((1 - ny) * 0.5f) * static_cast<float>(winH);
    return true;
}

// keys.h name of a key message (scan code first, the virtual key when the scan code is unknown); "" if neither.
inline std::string KeyNameOf(LPARAM lp, WPARAM vk) {
    const uint8_t scan = static_cast<uint8_t>((lp >> 16) & 0xFF);
    const bool ext = ((lp >> 24) & 1) != 0;
    const uint8_t dik = static_cast<uint8_t>((scan & 0x7F) | (ext ? 0x80 : 0));
    const char* n = dik ? DikName(dik) : "?";
    if (n[0] != '?') return n;
    for (const auto& k : melange::automation::kKeys)
        if (k.vk == static_cast<uint8_t>(vk) && vk < 0x100) return k.name;
    return {};
}

// Regions, pointer mode, keyboard focus and the button tracker: who gets each mouse and key message. Pure: the
// caller passes the cursor, the frame serial and the time. Not thread-safe (the main thread owns all of it).
//
// Priority: the overlay (the caller never routes what it captures), then the pointer or focus owner, then regions
// (higher load order first, then the most recently claimed), then the game. The game always gets the release of a
// button whose press it saw; a press a mod took keeps its release from the game too.
class ModInput {
public:
    using Result = melange::render::modinput::Result;
    using Shape = melange::render::modinput::Shape;
    struct Point {
        bool valid = false;
        float x = 0, y = 0;
    };
    struct Region {
        std::string mod, id;
        int order = 0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        uint64_t until = 0;  // live while frame < until
        uint64_t since = 0;  // ms: claimed continuously, at this rect, since then
        uint64_t seq = 0;    // claim order: a later claim is on top
        Shape shape = Shape::Arrow;
        bool Contains(const Point& p) const { return p.valid && p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1; }
    };
    struct Out {
        bool toGame = true;
        bool endPointer = false, endFocus = false;  // the caller ends them (pointer mode has a zero-delta exit)
        bool paste = false;                         // Ctrl+V while focused: the caller reads the clipboard
        std::vector<ModEvent> events;
        std::vector<std::pair<std::string, bool>> marks;  // (mod, consentGrade): a click was delivered
    };

    static constexpr size_t kMaxRegionsPerMod = 256;
    static constexpr uint64_t kConsentMs = 500, kEscLatchMs = 1000, kExitGraceMs = 150;

    void SetDoubleClick(uint32_t ms, int dx, int dy) {
        dblMs_ = ms;
        dblDx_ = dx;
        dblDy_ = dy;
    }

    // ---- wum.input
    Result Claim(const std::string& mod, int order, const std::string& id, float x0, float y0, float x1, float y1,
                 int frames, Shape shape, uint64_t frame, uint64_t nowMs, bool overlay) {
        if (overlay || OtherOwner(mod)) return Result::Busy;
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        if (frames < 1) frames = 1;
        size_t mine = 0;
        Region* same = nullptr;
        for (Region& r : regions_) {
            if (r.mod == mod) {
                ++mine;
                if (r.id == id) same = &r;
                continue;
            }
            if (r.order > order && frame < r.until && r.x0 < x1 && x0 < r.x1 && r.y0 < y1 && y0 < r.y1)
                return Result::Busy;  // a newer mod's live region overlaps: it takes those clicks
        }
        if (!same) {
            if (mine >= kMaxRegionsPerMod) return Result::Busy;
            regions_.push_back(Region{});
            same = &regions_.back();
            same->mod = mod;
            same->id = id;
            same->since = nowMs;
        } else if (frame >= same->until || same->x0 != x0 || same->y0 != y0 || same->x1 != x1 || same->y1 != y1) {
            same->since = nowMs;  // lapsed or moved: the consent clock starts again
        }
        same->order = order;
        same->x0 = x0, same->y0 = y0, same->x1 = x1, same->y1 = y1;
        same->until = frame + static_cast<uint64_t>(frames);
        same->seq = ++seq_;
        same->shape = shape;
        return Result::Ok;
    }

    // The topmost live region under `p` (of `onlyMod` when given).
    const Region* Hit(const Point& p, uint64_t frame, const std::string* onlyMod = nullptr) const {
        const Region* best = nullptr;
        for (const Region& r : regions_) {
            if (frame >= r.until || !r.Contains(p) || (onlyMod && r.mod != *onlyMod)) continue;
            if (!best || r.order > best->order || (r.order == best->order && r.seq > best->seq)) best = &r;
        }
        return best;
    }

    // Drops regions that lapsed (renewal after this restarts their consent clock).
    void Expire(uint64_t frame) {
        for (size_t i = regions_.size(); i-- > 0;)
            if (frame >= regions_[i].until) regions_.erase(regions_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // Starts (or updates) pointer mode for `mod`. On a fresh start `held` gets each button the game holds: the caller
    // sends it a release, and the first release of that button to arrive (the real one or ours) reaches the game.
    Result StartPointer(const std::string& mod, bool clickOutside, unsigned texture, bool overlay, std::vector<int>* held) {
        if (overlay || OtherOwner(mod)) return Result::Busy;
        const bool fresh = pointer_.empty();
        pointer_ = mod;
        clickOutside_ = clickOutside;
        texture_ = texture;
        if (fresh) {
            exitUntil_ = 0;
            for (int b = 0; b < kBtnCount; ++b)
                if (game_[b]) {
                    game_[b] = false;
                    synth_[b] = true;
                    if (held) held->push_back(b);
                }
        }
        return Result::Ok;
    }
    void EndPointer() {
        pointer_.clear();
        texture_ = 0;
    }
    // After the exit: for a moment, drop mouse moves that are not at the warp centre (moves queued while the cursor was
    // still elsewhere would be a jump against the rebased position).
    void ArmExitGrace(uint64_t nowMs, int cx, int cy) {
        exitUntil_ = nowMs + kExitGraceMs;
        exitX_ = cx;
        exitY_ = cy;
    }

    Result StartFocus(const std::string& mod, bool overlay) {
        if (overlay || OtherOwner(mod)) return Result::Busy;
        focus_ = mod;
        return Result::Ok;
    }
    void EndFocus() {
        focus_.clear();
        hiSurrogate_ = 0;
    }

    // Everything of `mod` goes; a press it took still keeps its release from the game, delivered to nobody.
    void Forget(const std::string& mod) {
        for (size_t i = regions_.size(); i-- > 0;)
            if (regions_[i].mod == mod) regions_.erase(regions_.begin() + static_cast<std::ptrdiff_t>(i));
        for (Held& h : held_)
            if (h.active && h.mod == mod) h.mod.clear();
        if (last_.mod == mod) last_ = Last{};
        if (pointer_ == mod) EndPointer();
        if (focus_ == mod) EndFocus();
    }

    const std::string& PointerOwner() const { return pointer_; }
    const std::string& FocusOwner() const { return focus_; }
    unsigned PointerTexture() const { return texture_; }
    bool ClickOutside() const { return clickOutside_; }
    bool GameHolds(int b) const { return b >= 0 && b < kBtnCount && game_[b]; }
    size_t RegionCount() const { return regions_.size(); }

    void LatchEsc(uint64_t nowMs) { escUntil_ = nowMs + kEscLatchMs; }
    bool EscLatched(uint64_t nowMs) const { return nowMs < escUntil_; }

    // ---- window messages
    // `lx, ly`: the message's own position (client px). `cursor`: where the player sees the cursor (the pointer, or
    // the frontend cursor), used for hit tests; the message position is not, since the game warps the OS cursor.
    Out Mouse(UINT m, WPARAM wp, int lx, int ly, const Point& cursor, uint64_t frame, uint64_t nowMs, int winW, int winH) {
        Out o;
        const bool ptr = !pointer_.empty();
        if (m == WM_MOUSEMOVE) {
            if (ptr) {
                o.toGame = false;
            } else if (nowMs < exitUntil_) {
                if (lx == exitX_ && ly == exitY_)
                    exitUntil_ = 0;  // the game is at the centre now: later moves are real motion
                else
                    o.toGame = false;
            }
            return o;
        }
        const ButtonMsg b = DecodeButton(m, wp);
        if (b.button >= 0) {
            if (b.down)
                Down(b, cursor, frame, nowMs, winW, winH, &o);
            else
                Up(b.button, cursor, frame, &o);
            return o;
        }
        if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) {
            const Region* r = Hit(cursor, frame, ptr ? &pointer_ : nullptr);
            if (ptr || r) o.toGame = false;
            if (r && m == WM_MOUSEWHEEL) {
                ModEvent e;
                e.kind = ModEvent::Kind::Wheel;
                e.mod = r->mod;
                e.region = r->id;
                e.delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
                e.x = cursor.x;
                e.y = cursor.y;
                o.events.push_back(std::move(e));
            }
            return o;
        }
        if (ptr) o.toGame = false;
        return o;
    }

    // A mouse message the overlay kept from the game: a press it took is no longer ours to release.
    void OverlaySwallowed(UINT m, WPARAM wp) {
        const ButtonMsg b = DecodeButton(m, wp);
        if (b.button >= 0 && !b.down) held_[b.button] = Held{};
    }

    // The window lost focus: releases already in flight will not arrive. The game keeps what it thinks it holds.
    void LoseFocus() {
        for (Held& h : held_) h = Held{};
        last_ = Last{};
    }

    // Key messages that are not hotkeys. `mods`: kModCtrl | kModShift | kModAlt held now.
    Out Key(UINT m, WPARAM wp, LPARAM lp, uint8_t mods, uint64_t nowMs) {
        Out o;
        const bool down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN, up = m == WM_KEYUP || m == WM_SYSKEYUP;
        const bool repeat = down && (lp & (1 << 30)) != 0;
        if (!focus_.empty()) {
            if (down || up) {
                ModEvent e;
                e.kind = ModEvent::Kind::Key;
                e.mod = focus_;
                e.key = KeyNameOf(lp, wp);
                e.down = down;
                e.repeat = repeat;
                e.ctrl = (mods & kModCtrl) != 0;
                e.shift = (mods & kModShift) != 0;
                e.alt = (mods & kModAlt) != 0;
                if (!e.key.empty()) o.events.push_back(std::move(e));
                // WM_SYSKEY* still reach the window: Alt+F4 and the system menu keep working while a box has focus.
                if (m == WM_KEYDOWN || m == WM_KEYUP) o.toGame = false;
                if (down && !repeat && wp == VK_ESCAPE) {
                    o.endFocus = true;  // delivered first, then focus ends
                    LatchEsc(nowMs);
                } else if (m == WM_KEYDOWN && !repeat && wp == 'V' && (mods & kModCtrl) && !(mods & kModAlt)) {
                    o.paste = true;
                }
                return o;
            }
            if (m == WM_CHAR) {
                o.toGame = false;
                const uint32_t u = static_cast<uint16_t>(wp);
                uint32_t cp = u;
                if (u >= 0xD800 && u <= 0xDBFF) {
                    hiSurrogate_ = u;
                    return o;
                }
                if (u >= 0xDC00 && u <= 0xDFFF) {
                    if (!hiSurrogate_) return o;
                    cp = 0x10000 + ((hiSurrogate_ - 0xD800) << 10) + (u - 0xDC00);
                }
                hiSurrogate_ = 0;
                if (!TextCodepoint(cp)) return o;  // Enter, Backspace, Tab, Ctrl+letter: the key event has them
                ModEvent e;
                e.kind = ModEvent::Kind::Char;
                e.mod = focus_;
                AppendUtf8(e.text, cp);
                o.events.push_back(std::move(e));
                return o;
            }
            if (m == WM_DEADCHAR) o.toGame = false;
            return o;
        }
        if ((down || up) && wp == VK_ESCAPE) {
            if (!pointer_.empty() && down && !repeat) {
                o.toGame = false;  // Esc ends pointer mode and is consumed
                o.endPointer = true;
                LatchEsc(nowMs);
            } else if (EscLatched(nowMs)) {
                o.toGame = false;  // the rest of the Esc press that ended pointer mode or focus
            }
        }
        return o;
    }

private:
    struct Held {  // a press a mod took: its release is kept from the game
        bool active = false;
        std::string mod, region;  // mod "" = deliver to nobody (the click outside that ended pointer mode, an unload)
        bool consent = false;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    struct Last {  // the previous delivered press, for double-clicks
        std::string mod, region;
        int button = -1;
        uint64_t at = 0;
        float x = 0, y = 0;
    };

    bool OtherOwner(const std::string& mod) const {
        return (!pointer_.empty() && pointer_ != mod) || (!focus_.empty() && focus_ != mod);
    }

    void Down(const ButtonMsg& b, const Point& cursor, uint64_t frame, uint64_t nowMs, int winW, int winH, Out* o) {
        const bool ptr = !pointer_.empty();
        const Region* r = Hit(cursor, frame, ptr ? &pointer_ : nullptr);
        if (!r) {
            if (!ptr) {
                game_[b.button] = true;
                return;
            }
            o->toGame = false;
            held_[b.button] = Held{true, {}, {}, false, 0, 0, 0, 0};
            if (clickOutside_) o->endPointer = true;  // this click is consumed
            return;
        }
        o->toGame = false;
        const double area = static_cast<double>(r->x1 - r->x0) * static_cast<double>(r->y1 - r->y0);
        const bool consent = area <= 0.25 * static_cast<double>(winW) * static_cast<double>(winH) &&
                             nowMs >= r->since + kConsentMs;
        held_[b.button] = Held{true, r->mod, r->id, consent, r->x0, r->y0, r->x1, r->y1};
        ModEvent e;
        e.kind = ModEvent::Kind::Mouse;
        e.mod = r->mod;
        e.region = r->id;
        e.button = b.button;
        e.down = true;
        e.x = cursor.x;
        e.y = cursor.y;
        const float dx = cursor.x - last_.x, dy = cursor.y - last_.y;
        e.dbl = b.dbl || (last_.button == b.button && last_.mod == r->mod && last_.region == r->id &&
                          nowMs - last_.at <= dblMs_ && dx <= dblDx_ && -dx <= dblDx_ && dy <= dblDy_ && -dy <= dblDy_);
        last_ = e.dbl ? Last{} : Last{r->mod, r->id, b.button, nowMs, cursor.x, cursor.y};
        o->events.push_back(std::move(e));
    }

    void Up(int button, const Point& cursor, uint64_t frame, Out* o) {
        if (game_[button] || synth_[button]) {  // the game saw the press: its release always reaches it
            game_[button] = false;
            synth_[button] = false;
            return;
        }
        Held h = held_[button];
        if (!h.active) {
            if (!pointer_.empty()) o->toGame = false;
            return;
        }
        held_[button] = Held{};
        o->toGame = false;
        if (h.mod.empty()) return;
        ModEvent e;
        e.kind = ModEvent::Kind::Mouse;
        e.mod = h.mod;
        e.region = h.region;
        e.button = button;
        e.down = false;
        e.x = cursor.x;
        e.y = cursor.y;
        o->events.push_back(std::move(e));
        // A click: press and release inside the same region (as it is now, or as it was at the press).
        bool inside = cursor.valid && cursor.x >= h.x0 && cursor.x < h.x1 && cursor.y >= h.y0 && cursor.y < h.y1;
        if (!inside)
            for (const Region& r : regions_)
                if (r.mod == h.mod && r.id == h.region && frame < r.until && r.Contains(cursor)) inside = true;
        if (inside) o->marks.emplace_back(h.mod, h.consent);
    }

    std::vector<Region> regions_;
    uint64_t seq_ = 0;
    std::string pointer_, focus_;
    bool clickOutside_ = true;
    unsigned texture_ = 0;
    bool game_[kBtnCount] = {};   // the game saw the press and not yet the release
    bool synth_[kBtnCount] = {};  // a release was sent to the game for it (pointer start): let one release through
    Held held_[kBtnCount];
    Last last_;
    uint32_t dblMs_ = 500;
    int dblDx_ = 4, dblDy_ = 4;
    uint64_t escUntil_ = 0;
    uint64_t exitUntil_ = 0;
    int exitX_ = 0, exitY_ = 0;
    uint32_t hiSurrogate_ = 0;
};
}  // namespace melange::render
