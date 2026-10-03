#include "launcher/setup/ini_merge.h"

#include <map>
#include <set>
#include <vector>

#include "launcher/util.h"

namespace melange::launcher::setup {
namespace {
struct Line {
    std::string text;   // without the line ending
    std::string eol;    // "\r\n", "\n" or "" (last line)
};

std::vector<Line> Split(std::string_view s) {
    std::vector<Line> out;
    size_t i = 0;
    while (i < s.size()) {
        const size_t nl = s.find('\n', i);
        if (nl == std::string_view::npos) {
            out.push_back(Line{std::string(s.substr(i)), ""});
            break;
        }
        const bool cr = nl > i && s[nl - 1] == '\r';
        out.push_back(Line{std::string(s.substr(i, nl - i - (cr ? 1 : 0))), cr ? "\r\n" : "\n"});
        i = nl + 1;
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool IsComment(std::string_view t) {
    t = Trim(t);
    return !t.empty() && (t[0] == ';' || t[0] == '#');
}
bool IsBlank(std::string_view t) { return Trim(t).empty(); }
// "[name]" -> name (lower-case), else "".
bool SectionOf(std::string_view t, std::string* name) {
    t = Trim(t);
    if (t.size() < 2 || t[0] != '[') return false;
    const size_t close = t.find(']');
    if (close == std::string_view::npos) return false;
    *name = Lower(std::string(Trim(t.substr(1, close - 1))));
    return true;
}
bool KeyOf(std::string_view t, std::string* key) {
    if (IsComment(t) || IsBlank(t)) return false;
    const size_t eq = t.find('=');
    if (eq == std::string_view::npos) return false;
    const std::string_view k = Trim(t.substr(0, eq));
    if (k.empty() || k[0] == '[') return false;
    *key = Lower(std::string(k));
    return true;
}

struct TKey {
    std::string key;
    std::vector<std::string> lines;   // comments directly above, then the key line
};
struct TSection {
    std::string name;
    std::vector<std::string> chunk;   // comments above the header, the header, the body (trailing blanks dropped)
    std::vector<TKey> keys;
};

std::vector<TSection> ParseTemplate(std::string_view text) {
    const auto lines = Split(text);
    std::vector<TSection> out;
    std::vector<size_t> headers;
    std::string name;
    for (size_t i = 0; i < lines.size(); ++i)
        if (SectionOf(lines[i].text, &name)) headers.push_back(i);
    auto commentStart = [&](size_t at) {
        size_t s = at;
        while (s > 0 && IsComment(lines[s - 1].text)) --s;
        return s;
    };
    for (size_t h = 0; h < headers.size(); ++h) {
        TSection sec;
        SectionOf(lines[headers[h]].text, &sec.name);
        const size_t begin = commentStart(headers[h]);
        size_t end = h + 1 < headers.size() ? commentStart(headers[h + 1]) : lines.size();
        while (end > headers[h] + 1 && IsBlank(lines[end - 1].text)) --end;
        for (size_t i = begin; i < end; ++i) sec.chunk.push_back(lines[i].text);
        std::vector<std::string> pending;
        for (size_t i = headers[h] + 1; i < end; ++i) {
            std::string key;
            if (IsComment(lines[i].text)) {
                pending.push_back(lines[i].text);
            } else if (KeyOf(lines[i].text, &key)) {
                TKey k;
                k.key = key;
                k.lines = pending;
                k.lines.push_back(lines[i].text);
                sec.keys.push_back(std::move(k));
                pending.clear();
            } else {
                pending.clear();
            }
        }
        out.push_back(std::move(sec));
    }
    return out;
}

struct USection {
    size_t lastKey = 0;   // the line to insert after (the header when the section has no key yet)
    std::set<std::string> keys;
};

std::map<std::string, USection> ParseUser(const std::vector<Line>& lines) {
    std::map<std::string, USection> out;
    std::set<std::string> done;
    std::string cur;
    bool active = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string name, key;
        if (SectionOf(lines[i].text, &name)) {
            active = !out.count(name);
            cur = name;
            if (active) out[name].lastKey = i;
        } else if (active && KeyOf(lines[i].text, &key)) {
            out[cur].keys.insert(key);
            out[cur].lastKey = i;
        }
    }
    return out;
}
}  // namespace

int IniMissingKeys(std::string_view templ, std::string_view user) {
    const auto t = ParseTemplate(templ);
    const auto u = ParseUser(Split(user));
    int n = 0;
    for (const auto& s : t) {
        auto it = u.find(s.name);
        for (const auto& k : s.keys)
            if (it == u.end() || !it->second.keys.count(k.key)) ++n;
    }
    return n;
}

std::string IniMerge(std::string_view templ, std::string_view user, int* added) {
    if (added) *added = 0;
    if (Trim(user).empty() && user.find('\n') == std::string_view::npos) {
        if (added) *added = IniMissingKeys(templ, user);
        return std::string(templ);
    }
    std::vector<Line> lines = Split(user);
    const std::string eol = std::string(user).find("\r\n") != std::string::npos || user.find('\n') == std::string_view::npos ? "\r\n" : "\n";
    const auto tsecs = ParseTemplate(templ);
    auto usecs = ParseUser(lines);
    // Inserts per user line index, applied in one pass so earlier indices stay valid.
    std::map<size_t, std::vector<std::string>> inserts;
    std::vector<std::string> appended;
    int count = 0;
    for (const auto& s : tsecs) {
        auto it = usecs.find(s.name);
        if (it == usecs.end()) {
            appended.push_back("");
            for (const auto& l : s.chunk) appended.push_back(l);
            count += static_cast<int>(s.keys.size());
            continue;
        }
        for (const auto& k : s.keys) {
            if (it->second.keys.count(k.key)) continue;
            auto& v = inserts[it->second.lastKey];
            for (const auto& l : k.lines) v.push_back(l);
            ++count;
        }
    }
    if (added) *added = count;
    if (!count) return std::string(user);
    std::string out;
    out.reserve(user.size() + templ.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i].text;
        auto ins = inserts.find(i);
        const bool last = i + 1 == lines.size();
        if (ins == inserts.end()) {
            out += lines[i].eol;
            continue;
        }
        out += lines[i].eol.empty() ? eol : lines[i].eol;
        for (size_t j = 0; j < ins->second.size(); ++j) {
            out += ins->second[j];
            if (j + 1 < ins->second.size() || !last || !lines[i].eol.empty()) out += eol;
        }
    }
    if (!appended.empty()) {
        if (!out.empty() && out.back() != '\n') out += eol;
        while (!appended.empty() && appended.front().empty() && (out.empty() || out.size() >= 2 * eol.size() &&
               out.compare(out.size() - 2 * eol.size(), 2 * eol.size(), eol + eol) == 0))
            appended.erase(appended.begin());
        for (const auto& l : appended) out += l + eol;
    }
    return out;
}
}  // namespace melange::launcher::setup
