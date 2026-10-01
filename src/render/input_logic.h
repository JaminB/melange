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
#include <vector>

#include "core/keys.h"

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
}  // namespace melange::render
