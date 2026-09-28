#pragma once
// Key-name tables for the Automation module: name -> DirectInput scan code (DIK_*) + Win32 virtual key.
#include <cstdint>
#include <cstring>

namespace melange::automation {
struct KeyDef {
    const char* name;
    uint8_t dik;
    uint8_t vk;
};

// clang-format off
inline constexpr KeyDef kKeys[] = {
    {"ESCAPE", 0x01, 0x1B}, {"ESC", 0x01, 0x1B},
    {"1", 0x02, '1'}, {"2", 0x03, '2'}, {"3", 0x04, '3'}, {"4", 0x05, '4'}, {"5", 0x06, '5'},
    {"6", 0x07, '6'}, {"7", 0x08, '7'}, {"8", 0x09, '8'}, {"9", 0x0A, '9'}, {"0", 0x0B, '0'},
    {"MINUS", 0x0C, 0xBD}, {"EQUALS", 0x0D, 0xBB}, {"BACK", 0x0E, 0x08}, {"BACKSPACE", 0x0E, 0x08},
    {"TAB", 0x0F, 0x09},
    {"Q", 0x10, 'Q'}, {"W", 0x11, 'W'}, {"E", 0x12, 'E'}, {"R", 0x13, 'R'}, {"T", 0x14, 'T'},
    {"Y", 0x15, 'Y'}, {"U", 0x16, 'U'}, {"I", 0x17, 'I'}, {"O", 0x18, 'O'}, {"P", 0x19, 'P'},
    {"LBRACKET", 0x1A, 0xDB}, {"RBRACKET", 0x1B, 0xDD},
    {"RETURN", 0x1C, 0x0D}, {"ENTER", 0x1C, 0x0D},
    {"LCONTROL", 0x1D, 0xA2}, {"CTRL", 0x1D, 0xA2},
    {"A", 0x1E, 'A'}, {"S", 0x1F, 'S'}, {"D", 0x20, 'D'}, {"F", 0x21, 'F'}, {"G", 0x22, 'G'},
    {"H", 0x23, 'H'}, {"J", 0x24, 'J'}, {"K", 0x25, 'K'}, {"L", 0x26, 'L'},
    {"SEMICOLON", 0x27, 0xBA}, {"APOSTROPHE", 0x28, 0xDE}, {"GRAVE", 0x29, 0xC0},
    {"LSHIFT", 0x2A, 0xA0}, {"SHIFT", 0x2A, 0xA0}, {"BACKSLASH", 0x2B, 0xDC},
    {"Z", 0x2C, 'Z'}, {"X", 0x2D, 'X'}, {"C", 0x2E, 'C'}, {"V", 0x2F, 'V'}, {"B", 0x30, 'B'},
    {"N", 0x31, 'N'}, {"M", 0x32, 'M'},
    {"COMMA", 0x33, 0xBC}, {"PERIOD", 0x34, 0xBE}, {"SLASH", 0x35, 0xBF}, {"RSHIFT", 0x36, 0xA1},
    {"MULTIPLY", 0x37, 0x6A}, {"LMENU", 0x38, 0xA4}, {"ALT", 0x38, 0xA4}, {"SPACE", 0x39, 0x20},
    {"CAPITAL", 0x3A, 0x14},
    {"F1", 0x3B, 0x70}, {"F2", 0x3C, 0x71}, {"F3", 0x3D, 0x72}, {"F4", 0x3E, 0x73}, {"F5", 0x3F, 0x74},
    {"F6", 0x40, 0x75}, {"F7", 0x41, 0x76}, {"F8", 0x42, 0x77}, {"F9", 0x43, 0x78}, {"F10", 0x44, 0x79},
    {"F11", 0x57, 0x7A}, {"F12", 0x58, 0x7B},
    {"NUMPAD7", 0x47, 0x67}, {"NUMPAD8", 0x48, 0x68}, {"NUMPAD9", 0x49, 0x69}, {"SUBTRACT", 0x4A, 0x6D},
    {"NUMPAD4", 0x4B, 0x64}, {"NUMPAD5", 0x4C, 0x65}, {"NUMPAD6", 0x4D, 0x66}, {"ADD", 0x4E, 0x6B},
    {"NUMPAD1", 0x4F, 0x61}, {"NUMPAD2", 0x50, 0x62}, {"NUMPAD3", 0x51, 0x63}, {"NUMPAD0", 0x52, 0x60},
    {"DECIMAL", 0x53, 0x6E},
    {"NUMPADENTER", 0x9C, 0x0D}, {"RCONTROL", 0x9D, 0xA3}, {"DIVIDE", 0xB5, 0x6F}, {"RMENU", 0xB8, 0xA5},
    {"PAUSE", 0xC5, 0x13}, {"HOME", 0xC7, 0x24}, {"UP", 0xC8, 0x26}, {"PRIOR", 0xC9, 0x21}, {"PGUP", 0xC9, 0x21},
    {"LEFT", 0xCB, 0x25}, {"RIGHT", 0xCD, 0x27}, {"END", 0xCF, 0x23}, {"DOWN", 0xD0, 0x28},
    {"NEXT", 0xD1, 0x22}, {"PGDN", 0xD1, 0x22}, {"INSERT", 0xD2, 0x2D}, {"DELETE", 0xD3, 0x2E},
};
// clang-format on

inline const KeyDef* FindKey(const char* name) {
    for (const auto& k : kKeys)
        if (_stricmp(k.name, name) == 0) return &k;
    return nullptr;
}

// Character -> key (+ whether shift must be held), for the `text` command. US layout.
inline const KeyDef* KeyForChar(char c, bool* shift) {
    static const char kUnshifted[] = "`1234567890-=[]\\;',./";
    static const char kShifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    static const char* kNames[] = {"GRAVE", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "MINUS", "EQUALS",
                                   "LBRACKET", "RBRACKET", "BACKSLASH", "SEMICOLON", "APOSTROPHE", "COMMA", "PERIOD",
                                   "SLASH"};
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        char n[2] = {static_cast<char>(c - 32), 0};
        return FindKey(n);
    }
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        char n[2] = {c, 0};
        return FindKey(n);
    }
    if (c == ' ') return FindKey("SPACE");
    for (int i = 0; kUnshifted[i]; ++i) {
        if (kUnshifted[i] == c) return FindKey(kNames[i]);
        if (kShifted[i] == c) {
            *shift = true;
            return FindKey(kNames[i]);
        }
    }
    return nullptr;
}
}  // namespace melange::automation
