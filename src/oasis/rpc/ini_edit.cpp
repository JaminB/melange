#include "oasis/rpc/ini_edit.h"

#include <windows.h>

#include <cstring>

namespace melange::oasis::ini {
namespace {
struct Line {
    std::string_view body, eol;
};

std::vector<Line> Split(std::string_view text) {
    std::vector<Line> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        if (nl == std::string_view::npos) {
            out.push_back({text.substr(i), {}});
            break;
        }
        size_t end = nl > i && text[nl - 1] == '\r' ? nl - 1 : nl;
        out.push_back({text.substr(i, end - i), text.substr(end, nl + 1 - end)});
        i = nl + 1;
    }
    return out;
}

bool Space(char c) { return c == ' ' || c == '\t'; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && Space(s.front())) s.remove_prefix(1);
    while (!s.empty() && Space(s.back())) s.remove_suffix(1);
    return s;
}

bool Same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// The section name of a "[name]" line, or false.
bool Header(std::string_view body, std::string_view* name) {
    std::string_view t = Trim(body);
    if (t.size() < 2 || t.front() != '[') return false;
    size_t close = t.find(']');
    if (close == std::string_view::npos) return false;
    *name = Trim(t.substr(1, close - 1));
    return true;
}

// Key text and the offset of the value in a "key = value" line, or false (comment, blank, no '=').
bool KeyLine(std::string_view body, std::string_view* key, size_t* valueAt) {
    std::string_view t = Trim(body);
    if (t.empty() || t.front() == ';' || t.front() == '[') return false;
    size_t eq = body.find('=');
    if (eq == std::string_view::npos) return false;
    *key = Trim(body.substr(0, eq));
    if (key->empty()) return false;
    size_t v = eq + 1;
    while (v < body.size() && Space(body[v])) ++v;
    *valueAt = v;
    return true;
}

std::string ValueView(std::string_view raw) {
    std::string_view v = Trim(raw);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    if (size_t c = v.find(';'); c != std::string_view::npos) v = v.substr(0, c);
    while (!v.empty() && Space(v.back())) v.remove_suffix(1);
    return std::string(v);
}

std::string_view Eol(std::string_view text) { return text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n"; }
}  // namespace

std::vector<Entry> Parse(std::string_view text) {
    std::vector<Entry> out;
    std::string section;
    bool inSection = false;
    int n = 0;
    for (const Line& l : Split(text)) {
        ++n;
        std::string_view name, key;
        size_t at = 0;
        if (Header(l.body, &name)) {
            section = std::string(name);
            inSection = true;
        } else if (inSection && KeyLine(l.body, &key, &at)) {
            if (!Find(out, section, key)) out.push_back({section, std::string(key), ValueView(l.body.substr(at)), n});
        }
    }
    return out;
}

const Entry* Find(const std::vector<Entry>& all, std::string_view section, std::string_view key) {
    for (const Entry& e : all)
        if (Same(e.section, section) && Same(e.key, key)) return &e;
    return nullptr;
}

bool ValidName(std::string_view s, std::string* why) {
    if (s.empty() || s.size() > 128) return *why = "a name must have 1 to 128 characters", false;
    if (Space(s.front()) || Space(s.back())) return *why = "a name cannot start or end with a space", false;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f || c == '[' || c == ']' || c == '=' || c == ';')
            return *why = "a name cannot contain control characters, '[', ']', '=' or ';'", false;
    return true;
}

bool ValidValue(std::string_view s, std::string* why) {
    if (s.size() > 1000) return *why = "the value is longer than 1000 characters", false;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == 0) return *why = "the value cannot contain line breaks", false;
        if (c < 0x20 && c != '\t') return *why = "the value cannot contain control characters", false;
        if (c == ';') return *why = "';' starts a comment in Melange.ini, so the value cannot contain it", false;
    }
    if (!s.empty() && (Space(s.front()) || Space(s.back()))) return *why = "the value cannot start or end with a space", false;
    return true;
}

bool Protected(std::string_view section, std::string_view key, std::string_view value, std::string* why) {
    if (!Same(section, "Thumper")) return false;
    if (Same(key, "GrantSalt")) return *why = "the Deep Desert grant salt can only be changed in Melange.ini itself", true;
    if (Same(key, "AutoGrantDeepDesert") && ValueView(value) != "0")
        return *why = "Deep Desert can be revoked from the browser but never granted", true;
    return false;
}

std::string Set(std::string_view text, std::string_view section, std::string_view key, std::string_view value) {
    const std::vector<Line> lines = Split(text);
    const std::string_view eol = Eol(text);
    int header = -1, lastKey = -1, hit = -1;
    size_t hitAt = 0;
    bool in = false;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string_view name, k;
        size_t at = 0;
        if (Header(lines[i].body, &name)) {
            if (in) break;
            in = Same(name, section);
            if (in) header = lastKey = i;
        } else if (in && KeyLine(lines[i].body, &k, &at)) {
            lastKey = i;
            if (Same(k, key)) {
                hit = i;
                hitAt = at;
                break;
            }
        }
    }
    std::string out;
    out.reserve(text.size() + key.size() + value.size() + section.size() + 8);
    const std::string newLine = std::string(key) + "=" + std::string(value);
    auto emit = [&](int from, int to) {
        for (int i = from; i < to; ++i) {
            out += lines[i].body;
            out += lines[i].eol;
        }
    };
    if (hit >= 0) {
        const std::string_view body = lines[hit].body;
        const std::string_view rest = body.substr(hitAt);
        size_t c = rest.find(';');
        std::string_view comment;
        if (c != std::string_view::npos) {
            while (c > 0 && Space(rest[c - 1])) --c;
            comment = rest.substr(c);
            if (value.empty() && !comment.empty() && Space(comment.front())) comment.remove_prefix(1);
        }
        emit(0, hit);
        out += body.substr(0, hitAt);
        out += value;
        out += comment;
        out += lines[hit].eol;
        emit(hit + 1, static_cast<int>(lines.size()));
        return out;
    }
    if (header >= 0) {
        emit(0, lastKey + 1);
        if (lines[lastKey].eol.empty()) out += eol;
        out += newLine;
        out += lines[lastKey].eol.empty() ? std::string_view{} : eol;
        emit(lastKey + 1, static_cast<int>(lines.size()));
        return out;
    }
    out.assign(text);
    if (!out.empty() && out.back() != '\n') out += eol;
    if (!out.empty()) out += eol;
    out += "[" + std::string(section) + "]";
    out += eol;
    out += newLine;
    out += eol;
    return out;
}

std::string Decode(std::string_view b, Encoding* enc) {
    if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0xff && static_cast<unsigned char>(b[1]) == 0xfe) {
        *enc = Encoding::Utf16Le;
        const size_t n = (b.size() - 2) / 2;
        std::wstring w(n, L'\0');
        if (n) memcpy(w.data(), b.data() + 2, n * 2);
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len > 0 ? len : 0), '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
        return s;
    }
    if (b.size() >= 3 && b.substr(0, 3) == "\xEF\xBB\xBF") {
        *enc = Encoding::Utf8Bom;
        return std::string(b.substr(3));
    }
    *enc = Encoding::Ansi;
    if (b.empty()) return {};
    const int wl = MultiByteToWideChar(CP_ACP, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(wl > 0 ? wl : 0), L'\0');
    if (wl > 0) MultiByteToWideChar(CP_ACP, 0, b.data(), static_cast<int>(b.size()), w.data(), wl);
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

bool Encode(std::string_view utf8, Encoding enc, std::string* out) {
    out->clear();
    if (enc == Encoding::Utf8Bom) {
        *out = "\xEF\xBB\xBF" + std::string(utf8);
        return true;
    }
    std::wstring w;
    if (!utf8.empty()) {
        const int wl = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wl <= 0) return false;
        w.resize(static_cast<size_t>(wl));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), w.data(), wl);
    }
    if (enc == Encoding::Utf16Le) {
        out->assign("\xFF\xFE", 2);
        out->append(reinterpret_cast<const char*>(w.data()), w.size() * 2);
        return true;
    }
    if (w.empty()) return true;
    BOOL lossy = FALSE;
    const int len = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, &lossy);
    if (len <= 0 || lossy) return false;
    out->resize(static_cast<size_t>(len));
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), out->data(), len, nullptr, &lossy);
    return !lossy;
}
}  // namespace melange::oasis::ini
