#include "launcher/setup/vdf.h"

#include "launcher/util.h"

namespace melange::launcher::vdf {
namespace {
struct Lexer {
    std::string_view s;
    size_t i = 0;
    bool error = false;

    void Skip() {
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            return;
        }
    }
    // 0 end, '{', '}', 's' string (into *out)
    char Next(std::string* out) {
        Skip();
        if (i >= s.size()) return 0;
        const char c = s[i];
        if (c == '{' || c == '}') {
            ++i;
            return c;
        }
        out->clear();
        if (c == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char e = s[i + 1];
                    i += 2;
                    switch (e) {
                        case 'n': *out += '\n'; break;
                        case 't': *out += '\t'; break;
                        case '\\': *out += '\\'; break;
                        case '"': *out += '"'; break;
                        default: *out += '\\'; *out += e;
                    }
                    continue;
                }
                *out += s[i++];
            }
            if (i >= s.size()) {
                error = true;
                return 0;
            }
            ++i;
            return 's';
        }
        while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n' && s[i] != '{' && s[i] != '}' && s[i] != '"')
            *out += s[i++];
        return 's';
    }
};

// Parses children into `parent` until '}' (depth > 0) or the end (depth 0).
bool ParseBlock(Lexer& lx, Node* parent, int depth) {
    std::string key, val;
    for (;;) {
        const char t = lx.Next(&key);
        if (t == 0) return depth == 0 && !lx.error;
        if (t == '}') return depth > 0;
        if (t == '{') return false;
        const size_t save = lx.i;
        const char v = lx.Next(&val);
        if (v == 's') {
            Node n;
            n.key = key;
            n.value = val;
            parent->children.push_back(std::move(n));
            continue;
        }
        if (v == '{') {
            if (depth + 1 >= kMaxDepth) return false;
            Node n;
            n.key = key;
            n.block = true;
            const bool ok = ParseBlock(lx, &n, depth + 1);
            parent->children.push_back(std::move(n));
            if (!ok) return false;
            continue;
        }
        lx.i = save;
        return false;
    }
}
}  // namespace

const Node* Node::Get(std::string_view k) const {
    for (const auto& c : children)
        if (IEquals(c.key, k)) return &c;
    return nullptr;
}

std::string Node::Str(std::string_view k) const {
    const Node* n = Get(k);
    return n && !n->block ? n->value : std::string();
}

bool Parse(std::string_view text, Node* out) {
    *out = Node{};
    out->block = true;
    bool ok = true;
    if (text.size() > kMaxBytes) {
        text = text.substr(0, kMaxBytes);
        ok = false;
    }
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    Lexer lx{text};
    return ParseBlock(lx, out, 0) && ok;
}

std::vector<std::string> Libraries(const Node& root, std::string_view appId) {
    std::vector<std::string> first, rest;
    const Node* lf = root.Get("libraryfolders");
    if (!lf || !lf->block) return {};
    for (const auto& c : lf->children) {
        bool digits = !c.key.empty();
        for (char ch : c.key) digits &= ch >= '0' && ch <= '9';
        if (!digits) continue;
        if (!c.block) {
            if (!c.value.empty()) rest.push_back(c.value);
            continue;
        }
        const std::string path = c.Str("path");
        if (path.empty()) continue;
        const Node* apps = c.Get("apps");
        (apps && apps->block && apps->Get(appId) ? first : rest).push_back(path);
    }
    first.insert(first.end(), rest.begin(), rest.end());
    return first;
}

std::string InstallDir(const Node& root) {
    const Node* st = root.Get("AppState");
    return st && st->block ? st->Str("installdir") : std::string();
}
}  // namespace melange::launcher::vdf
