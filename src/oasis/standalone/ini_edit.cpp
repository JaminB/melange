#include "oasis/standalone/ini_edit.h"

#include <cctype>
#include <string_view>
#include <vector>

namespace melange::oasis::standalone::ini {
namespace {
struct Line { std::string text, ending; };  // ending: "", "\n" or "\r\n"

std::vector<Line> Split(const std::string& text) {
    std::vector<Line> out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t nl = text.find('\n', i);
        if (nl == std::string::npos) {
            out.push_back({text.substr(i), ""});
            break;
        }
        size_t end = nl;
        std::string ending = "\n";
        if (end > i && text[end - 1] == '\r') {
            --end;
            ending = "\r\n";
        }
        out.push_back({text.substr(i, end - i), ending});
        i = nl + 1;
    }
    return out;
}

std::string Join(const std::vector<Line>& lines) {
    std::string s;
    for (const auto& l : lines) {
        s += l.text;
        s += l.ending;
    }
    return s;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool IsSection(std::string_view trimmed, std::string_view section) {
    return trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']' &&
           trimmed.substr(1, trimmed.size() - 2) == section;
}

bool AnySection(std::string_view trimmed) { return trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']'; }
}  // namespace

std::string Get(const std::string& text, const std::string& section, const std::string& key) {
    bool inSection = false;
    for (const Line& l : Split(text)) {
        const std::string_view t = Trim(l.text);
        if (AnySection(t)) {
            inSection = IsSection(t, section);
            continue;
        }
        if (!inSection) continue;
        const size_t eq = l.text.find('=');
        if (eq == std::string::npos) continue;
        if (Trim(std::string_view(l.text).substr(0, eq)) != key) continue;
        std::string_view v = std::string_view(l.text).substr(eq + 1);
        const size_t semi = v.find(';');
        if (semi != std::string_view::npos) v = v.substr(0, semi);
        return std::string(Trim(v));
    }
    return "";
}

bool Set(const std::string& text, const std::string& section, const std::string& key, const std::string& value, std::string* out) {
    if (value.find_first_of("\r\n;") != std::string::npos) return false;
    std::vector<Line> lines = Split(text);
    int sectionLine = -1, keyLine = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string_view t = Trim(lines[i].text);
        if (AnySection(t)) {
            if (sectionLine >= 0) break;  // reached the next section without finding the key
            if (IsSection(t, section)) sectionLine = i;
            continue;
        }
        if (sectionLine < 0) continue;
        const size_t eq = lines[i].text.find('=');
        if (eq == std::string::npos) continue;
        if (Trim(std::string_view(lines[i].text).substr(0, eq)) == key) {
            keyLine = i;
            break;
        }
    }
    if (keyLine >= 0) {
        // Replace only the value's own span, so leading spacing and any inline comment keep their exact bytes.
        std::string& t = lines[keyLine].text;
        const size_t eq = t.find('=');
        size_t vs = eq + 1;
        while (vs < t.size() && (t[vs] == ' ' || t[vs] == '\t')) ++vs;
        const size_t semi = t.find(';', eq + 1);
        size_t ve = semi == std::string::npos ? t.size() : semi;
        while (ve > vs && (t[ve - 1] == ' ' || t[ve - 1] == '\t')) --ve;
        t = t.substr(0, vs) + value + t.substr(ve);
    } else if (sectionLine >= 0) {
        const std::string ending = lines[sectionLine].ending.empty() ? "\n" : lines[sectionLine].ending;
        lines.insert(lines.begin() + sectionLine + 1, Line{key + "=" + value, ending});
    } else {
        const std::string ending = lines.empty() || lines.back().ending.empty() ? "\n" : lines.back().ending;
        if (!lines.empty() && lines.back().ending.empty() && !lines.back().text.empty()) lines.back().ending = ending;
        lines.push_back(Line{"[" + section + "]", ending});
        lines.push_back(Line{key + "=" + value, ending});
    }
    *out = Join(lines);
    return true;
}
}  // namespace melange::oasis::standalone::ini
