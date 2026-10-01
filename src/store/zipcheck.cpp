#include "store/zipcheck.h"

#include <cctype>
#include <cstring>
#include <set>

namespace melange::store::zipcheck {
namespace {
std::string Lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

bool Fail(std::string* why, std::string_view name, const char* text) {
    *why = std::string(name.substr(0, 200)) + ": " + text;
    return false;
}

bool DeviceName(std::string_view seg) {
    std::string base = Lower(seg.substr(0, seg.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
    if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9') return true;
    return false;
}

bool SegmentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == ' ' || c == '-';
}
}  // namespace

bool ExecutableName(std::string_view name) {
    static const char* const kExt[] = {".exe", ".dll", ".asi", ".sys", ".scr", ".com", ".bat", ".cmd", ".ps1",
                                       ".psm1", ".vbs", ".wsf", ".hta", ".msi", ".jar", ".lnk", ".reg"};
    const std::string n = Lower(name);
    for (const char* e : kExt)
        if (n.size() > strlen(e) && n.ends_with(e)) return true;
    return false;
}

bool ExecutableMagic(const void* head, size_t n) {
    const auto* p = static_cast<const unsigned char*>(head);
    if (n >= 2 && p[0] == 'M' && p[1] == 'Z') return true;
    return n >= 4 && p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F';
}

bool CheckName(std::string_view name, std::string_view id, std::string* why) {
    if (name.empty()) return Fail(why, "(empty)", "empty name");
    if (name.size() > kMaxPathBytes) return Fail(why, name, "path longer than 180 bytes");
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f) return Fail(why, name, "control character");
    if (name.front() == '/' || name.front() == '\\') return Fail(why, name, "absolute path");
    if (name.find('\\') != std::string_view::npos) return Fail(why, name, "backslash in path");
    if (name.find(':') != std::string_view::npos) return Fail(why, name, "drive letter or ':' in path");
    std::string_view body = name;
    if (body.back() == '/') body.remove_suffix(1);
    std::vector<std::string_view> segs;
    size_t p = 0;
    while (p <= body.size()) {
        size_t q = body.find('/', p);
        if (q == std::string_view::npos) q = body.size();
        segs.push_back(body.substr(p, q - p));
        p = q + 1;
    }
    if (segs.size() > kMaxSegments) return Fail(why, name, "more than 8 folder levels");
    for (std::string_view s : segs) {
        if (s.empty()) return Fail(why, name, "empty path segment");
        if (s == "." || s == "..") return Fail(why, name, "'.' or '..' in path");
        if (s.front() == '.') return Fail(why, name, "hidden file or folder");
        if (s.back() == '.' || s.back() == ' ') return Fail(why, name, "segment ends with a dot or a space");
        for (char c : s)
            if (!SegmentChar(c)) return Fail(why, name, "character outside A-Z a-z 0-9 . _ - and space");
        if (DeviceName(s)) return Fail(why, name, "Windows device name");
        const std::string l = Lower(s);
        if (l == "thumper-state.json" || l == "storage.json" || l == "melange.ini") return Fail(why, name, "reserved file name");
    }
    if (segs.front() != id) return Fail(why, name, "outside the plugin's top folder");
    if (segs.size() >= 2 && Lower(segs[1]) == "user") return Fail(why, name, "user/ is reserved for the mod's own data");
    if (name.back() != '/' && ExecutableName(segs.back())) return Fail(why, name, "executable file type");
    return true;
}

bool Check(const std::vector<Entry>& entries, std::string_view id, uint64_t maxTotal, std::string* why) {
    if (entries.size() > kMaxEntries) {
        *why = "more than 2000 entries";
        return false;
    }
    std::set<std::string> folded;
    uint64_t total = 0;
    bool manifest = false;
    for (const Entry& e : entries) {
        if (!CheckName(e.name, id, why)) return false;
        const bool dir = e.name.back() == '/';
        if (e.flags & 0x41) return Fail(why, e.name, "encrypted entry");
        if (e.method != 0 && e.method != 8) return Fail(why, e.name, "compression method other than stored or deflate");
        const uint32_t mode = e.externalAttr >> 16;
        if ((e.madeBy >> 8) == 3 && mode) {
            const uint32_t type = mode & 0170000;
            if (type != 0100000 && type != 0040000) return Fail(why, e.name, "symbolic link or special file");
            if ((type == 0040000) != dir) return Fail(why, e.name, "file type does not match the name");
        }
        if (e.externalAttr & 0x400) return Fail(why, e.name, "reparse point");
        if (dir && e.size) return Fail(why, e.name, "folder entry with data");
        if (e.size > kMaxEntryBytes) return Fail(why, e.name, "file larger than 32 MiB");
        total += e.size;
        if (total > maxTotal) return Fail(why, e.name, "unpacked size above the store's record");
        std::string key = Lower(e.name);
        if (key.back() == '/') key.pop_back();
        if (!folded.insert(key).second) return Fail(why, e.name, "duplicate name (names are case-insensitive)");
        if (key == Lower(std::string(id) + "/spice.json")) manifest = true;
    }
    if (!manifest) {
        *why = std::string(id) + "/spice.json is missing";
        return false;
    }
    return true;
}
}  // namespace melange::store::zipcheck
