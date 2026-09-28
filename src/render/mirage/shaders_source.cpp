#include "render/mirage/shaders_source.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace melange::mirage::shadersrc {
namespace {
char Low(char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> Lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find('\n', p);
        if (q == std::string_view::npos) q = s.size();
        std::string_view l = s.substr(p, q - p);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        out.push_back(l);
        p = q + 1;
    }
    return out;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t p = 0;
    for (;;) {
        size_t q = s.find(sep, p);
        out.push_back(Trim(s.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p)));
        if (q == std::string_view::npos) break;
        p = q + 1;
    }
    return out;
}

bool ParseFloat(std::string_view s, float* out) {
    std::string t(Trim(s));
    if (t.empty()) return false;
    char* end = nullptr;
    *out = strtof(t.c_str(), &end);
    return end && *end == 0;
}

size_t Count(std::string_view hay, std::string_view needle, size_t* first) {
    size_t n = 0;
    *first = std::string_view::npos;
    for (size_t p = hay.find(needle); p != std::string_view::npos; p = hay.find(needle, p + needle.size())) {
        if (!n) *first = p;
        ++n;
    }
    return n;
}

// A relative shader path with backslashes and no leading separator.
std::wstring RelPath(std::string_view rel) {
    std::string r(rel);
    std::replace(r.begin(), r.end(), '/', '\\');
    size_t p = r.find_first_not_of('\\');
    return Widen(p == std::string::npos ? std::string_view{} : std::string_view(r).substr(p));
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
}  // namespace

std::string Lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = Low(c);
    return r;
}

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Low(a[i]) != Low(b[i])) return false;
    return true;
}

bool IContains(std::string_view s, std::string_view needle) {
    if (needle.empty()) return true;
    return Lower(s).find(Lower(needle)) != std::string::npos;
}

bool Glob(std::string_view pat, std::string_view s) {
    size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || Low(pat[p]) == Low(s[i]))) {
            ++p, ++i;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::string BaseName(std::string_view path) {
    size_t p = path.find_last_of("/\\");
    return std::string(p == std::string_view::npos ? path : path.substr(p + 1));
}

std::string CrlfToLf(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n')) r += s[i];
    return r;
}

std::vector<std::string> ScanIncludes(std::string_view text) {
    std::vector<std::string> out;
    for (std::string_view l : Lines(text)) {
        l = Trim(l);
        if (l.empty() || l[0] != '#') continue;
        l = Trim(l.substr(1));
        if (l.substr(0, 7) != "include") continue;
        l = Trim(l.substr(7));
        if (l.empty() || (l[0] != '"' && l[0] != '<')) continue;
        size_t e = l.find(l[0] == '"' ? '"' : '>', 1);
        if (e != std::string_view::npos && e > 1) out.emplace_back(l.substr(1, e - 1));
    }
    return out;
}

bool ParsePatch(std::string_view text, Patch* out, std::string* error) {
    *out = {};
    enum { kOutside, kFind, kReplace } state = kOutside;
    std::string find, replace;
    bool firstLine = true;
    int blockLine = 0, n = 0;
    auto fail = [&](int line, const char* what) {
        if (error) *error = "line " + std::to_string(line) + ": " + what;
        return false;
    };
    for (std::string_view l : Lines(text)) {
        ++n;
        std::string_view t = Trim(l);
        bool directive = t.size() >= 2 && t.substr(0, 2) == "@@";
        if (!directive) {
            if (state == kFind) find += std::string(l) + '\n';
            else if (state == kReplace) replace += std::string(l) + '\n';
            continue;
        }
        std::string_view d = Trim(t.substr(2));
        if (d.substr(0, 5) == "entry" && (d.size() == 5 || d[5] == ' ' || d[5] == '\t')) {
            if (!firstLine) return fail(n, "'@@ entry' must come before the first block");
            out->entryGlob = std::string(Trim(d.substr(5)));
            if (out->entryGlob.empty()) return fail(n, "'@@ entry' needs a pattern");
        } else if (d == "find") {
            if (state != kOutside) return fail(n, "'@@ find' inside a block");
            state = kFind;
            find.clear();
            replace.clear();
            blockLine = n;
        } else if (d == "replace") {
            if (state != kFind) return fail(n, "'@@ replace' without '@@ find'");
            state = kReplace;
        } else if (d == "end") {
            if (state != kReplace) return fail(n, "'@@ end' without '@@ replace'");
            if (!find.empty()) find.pop_back();
            if (!replace.empty()) replace.pop_back();
            if (find.empty()) return fail(blockLine, "empty '@@ find' block");
            out->blocks.push_back({find, replace, blockLine});
            state = kOutside;
        } else {
            return fail(n, "unknown directive");
        }
        firstLine = false;
    }
    if (state != kOutside) return fail(blockLine, "block not closed with '@@ end'");
    if (out->blocks.empty()) return fail(1, "no '@@ find' blocks");
    return true;
}

bool ApplyPatch(const Patch& p, std::string* text, int* bad, int* matches) {
    std::string t = *text;
    for (size_t i = 0; i < p.blocks.size(); ++i) {
        size_t first;
        size_t n = Count(t, p.blocks[i].find, &first);
        if (n != 1) {
            if (bad) *bad = static_cast<int>(i);
            if (matches) *matches = static_cast<int>(n);
            return false;
        }
        t.replace(first, p.blocks[i].find.size(), p.blocks[i].replace);
    }
    *text = std::move(t);
    return true;
}

std::vector<Diag> ParseListing(std::string_view listing, std::string_view mainFile) {
    std::vector<Diag> out;
    for (std::string_view l : Lines(listing)) {
        l = Trim(l);
        if (l.empty()) continue;
        Diag d{std::string(mainFile), 0, false, std::string(l)};
        size_t colon = l.find(") : ");
        size_t open = colon == std::string_view::npos ? colon : l.rfind('(', colon);
        if (open != std::string_view::npos) {
            std::string_view num = l.substr(open + 1, colon - open - 1);
            bool digits = !num.empty() && std::all_of(num.begin(), num.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (digits) {
                std::string_view file = Trim(l.substr(0, open));
                while (!file.empty() && (file.front() == '/' || file.front() == '\\')) file.remove_prefix(1);
                if (!file.empty()) d.file = std::string(file);
                d.line = atoi(std::string(num).c_str());
                std::string_view rest = Trim(l.substr(colon + 4));
                size_t sp = rest.find(' ');
                std::string_view kind = rest.substr(0, sp == std::string_view::npos ? 0 : sp);
                if (kind == "fatal") {
                    rest = Trim(rest.substr(sp));
                    sp = rest.find(' ');
                    kind = "error";
                }
                d.error = kind == "error";
                d.text = std::string(sp == std::string_view::npos ? rest : Trim(rest.substr(sp)));
            }
        }
        if (d.line == 0 && IContains(l, "error")) d.error = true;
        out.push_back(std::move(d));
    }
    return out;
}

bool ParseParams(std::string_view ini, std::vector<ParamSpec>* out, std::string* error) {
    std::string file, entry;
    int n = 0;
    auto fail = [&](const std::string& what) {
        if (error) *error = "line " + std::to_string(n) + ": " + what;
        return false;
    };
    for (std::string_view l : Lines(ini)) {
        ++n;
        l = Trim(l);
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[') {
            size_t e = l.find(']');
            if (e == std::string_view::npos) return fail("unterminated section");
            std::string_view s = Trim(l.substr(1, e - 1));
            size_t c = s.find(':');
            if (c == std::string_view::npos || c == 0 || c + 1 >= s.size()) return fail("section must be [File.cg:Entry]");
            file = std::string(Trim(s.substr(0, c)));
            entry = std::string(Trim(s.substr(c + 1)));
            continue;
        }
        if (file.empty()) return fail("parameter outside a [File.cg:Entry] section");
        size_t eq = l.find('=');
        if (eq == std::string_view::npos || eq == 0) return fail("expected name=type,default,min,max");
        ParamSpec p{};
        p.file = file;
        p.entryGlob = entry;
        p.name = std::string(Trim(l.substr(0, eq)));
        std::vector<std::string_view> f = Split(l.substr(eq + 1), ',');
        p.type = Lower(f[0]);
        p.n = p.type == "float" ? 1 : p.type == "vec2" ? 2 : p.type == "vec3" || p.type == "color" ? 3 : p.type == "vec4" ? 4 : 0;
        if (!p.n) return fail("unknown type '" + std::string(f[0]) + "'");
        p.min = 0;
        p.max = 1;
        if (f.size() > 1 && !f[1].empty()) {
            std::vector<std::string_view> c = Split(f[1], ' ');
            c.erase(std::remove(c.begin(), c.end(), std::string_view{}), c.end());
            if (static_cast<int>(c.size()) != p.n) return fail("default needs " + std::to_string(p.n) + " values");
            for (int i = 0; i < p.n; ++i)
                if (!ParseFloat(c[i], &p.def[i])) return fail("bad number '" + std::string(c[i]) + "'");
        }
        if (f.size() > 2 && !f[2].empty() && !ParseFloat(f[2], &p.min)) return fail("bad min");
        if (f.size() > 3 && !f[3].empty() && !ParseFloat(f[3], &p.max)) return fail("bad max");
        if (f.size() > 4) p.label = std::string(f[4]);
        if (f.size() > 5) return fail("too many fields");
        out->push_back(p);
    }
    return true;
}

std::vector<CgVar> ParseVars(std::string_view compiled) {
    std::vector<CgVar> out;
    for (std::string_view l : Lines(compiled)) {
        if (l.substr(0, 6) == "//var ") l.remove_prefix(6);
        else if (l.substr(0, 5) == "#var ") l.remove_prefix(5);
        else continue;
        std::vector<std::string_view> f;
        for (size_t p = 0;;) {
            size_t q = l.find(" : ", p);
            f.push_back(Trim(l.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p)));
            if (q == std::string_view::npos) break;
            p = q + 3;
        }
        if (f.size() < 5) continue;
        size_t sp = f[0].rfind(' ');
        if (sp == std::string_view::npos) continue;
        CgVar v;
        v.type = std::string(Trim(f[0].substr(0, sp)));
        v.name = std::string(Trim(f[0].substr(sp + 1)));
        v.semantic = std::string(f[1]);
        std::string_view res = f[2];
        v.count = 1;
        if (size_t c = res.find(','); c != std::string_view::npos) {
            v.count = std::max(1, atoi(std::string(Trim(res.substr(c + 1))).c_str()));
            res = Trim(res.substr(0, c));
        }
        v.resource = std::string(res);
        v.used = f[4] == "1";
        out.push_back(std::move(v));
    }
    return out;
}

bool LacksExplicitLod(std::string_view profile) {
    return IEquals(profile, "arbfp1") || IEquals(profile, "fp20") || IEquals(profile, "fp30");
}

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadFile(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64ll << 20);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = size.QuadPart == 0 || (::ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    }
    CloseHandle(h);
    return ok;
}

std::wstring Sources::FindInRoots(std::string_view rel, std::string* owner) const {
    std::wstring r = RelPath(rel);
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        std::wstring p = it->dir + L"\\" + r;
        if (IsFile(p)) {
            if (owner) *owner = it->owner;
            return p;
        }
    }
    return {};
}

bool Sources::AnyBuiltin(std::string_view entry, std::string_view profile) const {
    if (!builtins) return false;
    for (const Builtin& b : Builtins())
        if (Glob(b.entryGlob, entry) && b.profileOk(profile)) return true;
    return false;
}

Loaded Sources::Load(std::string_view rel, std::string_view entry, std::string_view profile, std::vector<Issue>* issues) const {
    Loaded l;
    std::wstring r = RelPath(rel);
    std::string base = BaseName(rel), owner;
    std::wstring p = FindInRoots(rel, &owner);
    if (!p.empty() && ReadFile(p, &l.text)) {
        l.found = l.fromRoot = true;
        l.owner = owner;
    } else if (ReadFile(vanillaDir + L"\\" + r, &l.text)) {
        l.found = true;
    }
    if (!l.found) return l;
    auto issue = [&](const std::string& file, int line, const std::string& text, const std::string& who, bool error) {
        if (issues) issues->push_back({file, line, text, who, error});
    };
    bool normalized = false;
    auto apply = [&](const Patch& patch, const std::string& file, const std::string& who, bool quiet) {
        if (!normalized) {
            l.text = CrlfToLf(l.text);
            normalized = true;
        }
        int bad = 0, matches = 0;
        if (ApplyPatch(patch, &l.text, &bad, &matches)) return true;
        issue(file, patch.blocks[bad].line,
              "patch block " + std::to_string(bad + 1) + " for " + base + " matched " + std::to_string(matches) +
                  " times (needs exactly 1); patch skipped",
              who, !quiet);
        return false;
    };
    for (const Root& root : roots) {
        std::string text;
        if (!ReadFile(root.dir + L"\\" + r + L".patch", &text)) continue;
        std::string file = base + ".patch", err;
        Patch patch;
        if (!ParsePatch(text, &patch, &err)) {
            issue(file, 0, err, root.owner, true);
            continue;
        }
        if (!patch.entryGlob.empty() && !Glob(patch.entryGlob, entry)) continue;
        if (apply(patch, file, root.owner, false)) {
            l.patched = true;
            l.owner = root.owner;
        }
    }
    if (builtins)
        for (const Builtin& b : Builtins()) {
            if (!IEquals(base, b.file) || !Glob(b.entryGlob, entry) || !b.profileOk(profile)) continue;
            Patch patch;
            std::string err;
            if (!ParsePatch(b.text, &patch, &err)) continue;
            // A mod that replaced or patched the file owns it; the fix not matching is expected then.
            if (apply(patch, std::string("builtin:") + b.id, "builtin", l.fromRoot || l.patched)) {
                l.builtin = true;
                if (l.owner.empty()) l.owner = "builtin";
            }
        }
    return l;
}

std::vector<std::string> Sources::Dependencies(std::string_view rel) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::vector<std::string> todo{std::string(rel)};
    while (!todo.empty() && seen.size() < 64) {
        std::string cur = todo.back();
        todo.pop_back();
        if (!seen.insert(Lower(cur)).second) continue;
        std::string text;
        std::wstring p = FindInRoots(cur, nullptr);
        if (!(p.empty() ? ReadFile(vanillaDir + L"\\" + RelPath(cur), &text) : ReadFile(p, &text))) continue;
        size_t slash = cur.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? "" : cur.substr(0, slash + 1);
        for (std::string inc : ScanIncludes(text)) {
            while (inc.size() > 2 && inc.substr(0, 2) == "./") inc.erase(0, 2);
            std::string full = !inc.empty() && (inc[0] == '/' || inc[0] == '\\') ? inc.substr(1) : dir + inc;
            std::string b = Lower(BaseName(full));
            if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
            todo.push_back(full);
        }
    }
    return out;
}

bool Cg::Load(void* dll) {
    auto m = static_cast<HMODULE>(dll);
    ok = false;
    if (!m) return false;
    int missing = 0;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
        if (!fn) ++missing;
    };
    get(CreateContext, "cgCreateContext");
    get(DestroyContext, "cgDestroyContext");
    get(CreateProgram, "cgCreateProgram");
    get(DestroyProgram, "cgDestroyProgram");
    get(GetError, "cgGetError");
    get(GetErrorString, "cgGetErrorString");
    get(GetLastListing, "cgGetLastListing");
    get(SetCompilerIncludeCallback, "cgSetCompilerIncludeCallback");
    get(GetCompilerIncludeCallback, "cgGetCompilerIncludeCallback");
    get(SetCompilerIncludeString, "cgSetCompilerIncludeString");
    get(ProfileString, "cgGetProfileString");
    get(ProfileByName, "cgGetProfile");
    get(GetProgramString, "cgGetProgramString");
    get(GetNamedParameter, "cgGetNamedParameter");
    get(SetParameterValuefr, "cgSetParameterValuefr");
    get(GetParameterValuefr, "cgGetParameterValuefr");
    get(GetParameterResourceIndex, "cgGetParameterResourceIndex");
    get(GetParameterRows, "cgGetParameterRows");
    get(GetFirstLeafParameter, "cgGetFirstLeafParameter");
    get(GetNextLeafParameter, "cgGetNextLeafParameter");
    get(GetParameterName, "cgGetParameterName");
    get(GetParameterVariability, "cgGetParameterVariability");
    get(GetProgramProfile, "cgGetProgramProfile");
    ok = missing == 0;
    return ok;
}

void Cg::Drain() const {
    if (!GetError) return;
    for (int i = 0; i < 64 && GetError(); ++i) {
    }
}

namespace {
thread_local Job* t_job = nullptr;

void __cdecl IncludeCallback(CGcontext ctx, const char* name) {
    Job* j = t_job;
    if (!j || !name) return;
    std::string rel(name);
    while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
    Loaded l = j->src->Load(rel, j->entry, j->profile, &j->issues);
    if (!l.found) return;
    j->cg->SetCompilerIncludeString(ctx, name, l.text.c_str());
    j->vfs.emplace_back(name);
    std::string dep = Lower(BaseName(rel));
    if (std::find(j->deps.begin(), j->deps.end(), dep) == j->deps.end()) j->deps.push_back(dep);
    if (l.fromRoot || l.patched || l.builtin) {
        j->overridden = true;
        if (!l.owner.empty() && (j->owner.empty() || (j->owner == "builtin" && l.owner != "builtin"))) j->owner = l.owner;
    }
}
}  // namespace

CGprogram Compile(Job& job, CGcontext ctx, const std::string& text, int profile, const char** args) {
    const Cg& cg = *job.cg;
    Job* outer = t_job;
    t_job = &job;
    CGinclude prev = cg.GetCompilerIncludeCallback(ctx);
    cg.SetCompilerIncludeCallback(ctx, &IncludeCallback);
    CGprogram p = cg.CreateProgram(ctx, kCgSource, text.c_str(), profile, job.entry.c_str(), args);
    // Virtual include files persist in the context and shadow the CG folder for later file-based compiles.
    for (const std::string& n : job.vfs) cg.SetCompilerIncludeString(ctx, n.c_str(), nullptr);
    job.vfs.clear();
    cg.SetCompilerIncludeCallback(ctx, prev);
    t_job = outer;
    return p;
}
}  // namespace melange::mirage::shadersrc
