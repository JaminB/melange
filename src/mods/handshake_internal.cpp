#include "mods/handshake_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "tools/hash.h"

namespace melange::handshake {
namespace {
const std::string kEllipsis = "\xE2\x80\xA6";

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool IsHex16(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string StripEllipsis(std::string v) {
    if (v.size() >= kEllipsis.size() && v.compare(v.size() - kEllipsis.size(), kEllipsis.size(), kEllipsis) == 0)
        v.resize(v.size() - kEllipsis.size());
    return v;
}

std::string Truncated(const std::vector<std::string>& items, size_t maxBytes) {
    std::string full;
    for (size_t i = 0; i < items.size(); ++i) full += (i ? "," : "") + items[i];
    if (full.size() <= maxBytes) return full;
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        std::string piece = (i ? "," : "") + items[i];
        if (out.size() + piece.size() + kEllipsis.size() > maxBytes) break;
        out += piece;
    }
    return out + kEllipsis;
}

void Add(std::string& s, const std::string& item) { s += (s.empty() ? "" : ", ") + item; }
}  // namespace

std::string SetNumber(double v) {
    char b[40];
    snprintf(b, sizeof(b), "%.17g", v);
    return b;
}

std::string SetBool(bool v) { return v ? "true" : "false"; }

std::string SetString(const std::string& v) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "\"";
    for (unsigned char c : v) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                           c == '_' || c == '/' || c == '-';
        if (plain) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out + "\"";
}

std::string CloneLine(const CloneSpec& c) {
    char head[64];
    snprintf(head, sizeof(head), "clone %u 0x%x ", static_cast<unsigned>(c.k), static_cast<unsigned>(c.vid));
    std::string t = head + c.name + " " + c.base + " " + std::to_string(c.cell) + " " +
                    (c.bankSha256.empty() ? "-" : c.bankSha256) + " ";
    auto set = c.set;
    std::stable_sort(set.begin(), set.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (set.empty()) t += "-";
    for (size_t i = 0; i < set.size(); ++i) t += (i ? " " : "") + set[i].first + "=" + set[i].second;
    return t;
}

std::string CanonicalText(const std::vector<ContentMod>& modsInLoadOrder, const std::vector<ModMessage>& messages,
                          const std::vector<CloneSpec>& clones, int extraPerExplosion) {
    std::string t = "melange-content/2\nsim=" + std::to_string(kSimApiVersion) + "\n";
    for (const auto& m : modsInLoadOrder) {
        t += "mod=" + m.id + "@" + m.version + "\n";
        for (const auto& f : m.files) t += "file=" + f.relPath + " " + f.sha256Hex + "\n";
    }
    t += "messages=" + std::to_string(messages.size()) + "\n";
    for (const auto& m : messages) t += "message=" + m.name + "=" + std::to_string(m.id) + "\n";
    for (const auto& c : clones) t += CloneLine(c) + "\n";
    if (!clones.empty()) t += "extras=" + std::to_string(extraPerExplosion) + "\n";
    return t;
}

std::string HashOfCanonicalText(const std::string& canonical) { return hashutil::Sha256Hex(canonical.data(), canonical.size()); }

mods::ContentId BuildContentId(std::vector<ContentMod> modsInLoadOrder, const std::vector<ModMessage>& messages,
                               const std::vector<CloneSpec>& clones, int extraPerExplosion) {
    for (auto& m : modsInLoadOrder)
        std::sort(m.files.begin(), m.files.end(), [](const ContentFile& a, const ContentFile& b) { return a.relPath < b.relPath; });
    mods::ContentId c{};
    c.contentMods = static_cast<uint32_t>(modsInLoadOrder.size());
    c.modMessages = static_cast<uint32_t>(messages.size());
    c.vanilla = modsInLoadOrder.empty() && messages.empty() && clones.empty();
    if (!c.vanilla) {
        std::string hash = HashOfCanonicalText(CanonicalText(modsInLoadOrder, messages, clones, extraPerExplosion));
        size_t n = std::min<size_t>(hash.size(), sizeof(c.hash) - 1);
        std::copy(hash.begin(), hash.begin() + static_cast<long>(n), c.hash);
        c.hash[n] = '\0';
    }
    return c;
}

std::string Hash16(const mods::ContentId& c) {
    if (c.vanilla || c.hash[0] == '\0') return "v";
    return std::string(c.hash).substr(0, 16);
}

std::string BuildMlgValue(const std::string& melangeVersion, const mods::ContentId& c) {
    return "1;" + melangeVersion + ";" + Hash16(c) + ";" + std::to_string(c.contentMods);
}

bool ParseMlgValue(const std::string& value, std::string* version, std::string* hash16, uint32_t* contentMods) {
    std::vector<std::string> parts = Split(value, ';');
    if (parts.size() != 4 || parts[0] != "1") return false;
    if (version) *version = parts[1];
    if (hash16) *hash16 = parts[2];
    if (contentMods) {
        char* end = nullptr;
        unsigned long v = strtoul(parts[3].c_str(), &end, 10);
        *contentMods = (end && *end == '\0') ? static_cast<uint32_t>(v) : 0;
    }
    return true;
}

std::string BuildModsValue(const std::vector<ContentMod>& modsInLoadOrder, size_t maxBytes) {
    std::vector<std::string> items;
    for (const auto& m : modsInLoadOrder) items.push_back(m.id + "@" + m.version);
    return Truncated(items, maxBytes);
}

namespace {
std::vector<std::pair<std::string, std::string>> ParsePairs(const std::string& value, char sep) {
    const std::string v = StripEllipsis(value);
    std::vector<std::pair<std::string, std::string>> out;
    size_t start = 0;
    while (start < v.size()) {
        size_t end = v.find(',', start);
        if (end == std::string::npos) end = v.size();
        std::string entry = v.substr(start, end - start);
        size_t at = entry.rfind(sep);
        if (!entry.empty()) out.emplace_back(entry.substr(0, at), at == std::string::npos ? "" : entry.substr(at + 1));
        start = end + 1;
    }
    return out;
}

const std::string* FindPair(const std::vector<std::pair<std::string, std::string>>& list, const std::string& key) {
    for (const auto& [k, v] : list)
        if (k == key) return &v;
    return nullptr;
}
}  // namespace

std::string DiffModsValues(const std::string& ours, const std::string& theirs) {
    const auto a = ParsePairs(ours, '@'), b = ParsePairs(theirs, '@');
    std::string missing, extra, versions;
    for (const auto& [id, v] : a) {
        const std::string* t = FindPair(b, id);
        if (!t) Add(missing, id + "@" + v);
        else if (*t != v) Add(versions, id + " " + v + " vs " + *t);
    }
    for (const auto& [id, v] : b)
        if (!FindPair(a, id)) Add(extra, id + "@" + v);
    std::string out;
    if (!missing.empty()) Add(out, "missing " + missing);
    if (!extra.empty()) Add(out, "extra " + extra);
    if (!versions.empty()) Add(out, versions);
    return out;
}

mods::PeerStatus ClassifyPeer(bool hasMlg, const std::string& theirHash16, const std::string& ourHash16) {
    if (!hasMlg) return mods::PeerStatus::Vanilla;
    if (theirHash16 == "v") return mods::PeerStatus::MelangeVanilla;
    return theirHash16 == ourHash16 ? mods::PeerStatus::Match : mods::PeerStatus::Mismatch;
}

std::string BuildMlgSim(const std::string& ourHash16, bool weAreVanilla, const std::vector<std::string>& memberHash16s) {
    if (weAreVanilla) return "";
    for (const auto& h : memberHash16s)
        if (h != ourHash16) return "off";
    return ourHash16;
}

bool GateAllowsSim(bool online, const std::string& ourHash16, const std::string& lobbySim) {
    if (!online) return true;
    return !ourHash16.empty() && ourHash16 != "v" && lobbySim == ourHash16;
}

std::string CloneHash16(const std::vector<CloneSpec>& clones, int extraPerExplosion) {
    if (clones.empty()) return "";
    std::string text;
    for (const auto& c : clones) text += CloneLine(c) + "\n";
    text += "extras=" + std::to_string(extraPerExplosion) + "\n";
    return HashOfCanonicalText(text).substr(0, 16);
}

std::string BuildWpnValue(const std::vector<CloneSpec>& clones, int extraPerExplosion) {
    if (clones.empty()) return "";
    return "1;" + CloneHash16(clones, extraPerExplosion) + ";" + std::to_string(clones.size());
}

bool ParseWpnValue(const std::string& value, std::string* hash16, uint32_t* clones) {
    const std::vector<std::string> parts = Split(value, ';');
    if (parts.size() != 3 || parts[0] != "1" || !IsHex16(parts[1])) return false;
    char* end = nullptr;
    const unsigned long n = strtoul(parts[2].c_str(), &end, 10);
    if (parts[2].empty() || !end || *end != '\0') return false;
    if (hash16) *hash16 = parts[1];
    if (clones) *clones = static_cast<uint32_t>(n);
    return true;
}

std::string BuildReqValue(const std::string& ourHash16, bool haveClones) {
    if (!haveClones || !IsHex16(ourHash16)) return "";
    return "wpn1;" + ourHash16;
}

bool ParseReqValue(const std::string& value, std::string* hash16) {
    const std::vector<std::string> parts = Split(value, ';');
    if (parts.size() != 2 || parts[0] != "wpn1" || !IsHex16(parts[1])) return false;
    if (hash16) *hash16 = parts[1];
    return true;
}

std::string BuildMsgValue(const std::vector<ModMessage>& messages, size_t maxBytes) {
    std::vector<std::string> items;
    for (const auto& m : messages) items.push_back(m.name + "=" + std::to_string(m.id));
    return Truncated(items, maxBytes);
}

std::string DiffMsgValues(const std::string& ours, const std::string& theirs) {
    const auto a = ParsePairs(ours, '='), b = ParsePairs(theirs, '=');
    std::string ids, missing, extra;
    for (const auto& [name, id] : a) {
        const std::string* t = FindPair(b, name);
        if (!t) Add(missing, name);
        else if (*t != id) Add(ids, name + " " + id + " vs " + *t);
    }
    for (const auto& [name, id] : b)
        if (!FindPair(a, name)) Add(extra, name);
    std::string out;
    if (!ids.empty()) Add(out, ids);
    if (!missing.empty()) Add(out, "missing " + missing);
    if (!extra.empty()) Add(out, "extra " + extra);
    if (!out.empty()) return "message ids differ: " + out;
    if (a != b) {
        std::string order;
        for (const auto& p : b) Add(order, p.first);
        return "message order differs: " + order;
    }
    return "";
}

std::vector<std::string> ChangedMessageIds(const std::vector<ModMessage>& hashed, uint16_t (*lookup)(const char*)) {
    std::vector<std::string> out;
    for (const auto& m : hashed)
        if (!lookup || lookup(m.name.c_str()) != m.id) out.push_back(m.name);
    return out;
}

CloneVerdict EvaluateCloneLobby(const CloneLobbyInput& in) {
    CloneVerdict v;
    if (!in.inLobby) return v;
    if (in.weAreOwner) {
        if (!in.haveClones) return v;
        for (const auto& m : in.members) {
            std::string reason;
            switch (ClassifyPeer(m.hasMlg, m.hash16, in.ourHash16)) {
                case mods::PeerStatus::Match: continue;
                case mods::PeerStatus::Vanilla: reason = "no Melange"; break;
                case mods::PeerStatus::MelangeVanilla: reason = "no content mods"; break;
                default: reason = m.diff.empty() ? "different mod files" : m.diff; break;
            }
            v.members.push_back((m.name.empty() ? std::string("?") : m.name) + ": " + reason);
        }
        if (v.members.empty()) return v;
        v.ok = false;
        v.hostHeld = true;
        for (const auto& s : v.members) v.why += (v.why.empty() ? "" : "; ") + s;
        return v;
    }
    const std::string diff = in.diffToHost.empty() ? "different mod files" : in.diffToHost;
    if (!in.lobbyReq.empty()) {
        std::string reqHash;
        const bool parsed = ParseReqValue(in.lobbyReq, &reqHash);
        if (parsed && reqHash == in.ourHash16) return v;
        v.ok = false;
        v.joinerMismatch = true;
        const std::string from = in.hostMods.empty() ? std::string("the host's mods") : StripEllipsis(in.hostMods);
        v.why = parsed ? "This lobby uses clone weapons from: " + from + ". You have: " + diff + "."
                       : "This lobby uses clone weapons from a newer Melange (" + in.lobbyReq + ").";
        return v;
    }
    if (in.haveClones) {
        v.ok = false;
        v.why = "The host has no clone weapons, so yours stay off this match.";
    }
    return v;
}

std::string LowerSlashes(std::string s) {
    for (char& c : s) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool GidHashesPath(const std::string& path) {
    const std::string p = LowerSlashes(path);
    return p.rfind("data/language/", 0) != 0 && p.rfind("data2/language/", 0) != 0;
}

std::string GidDataText(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<std::pair<std::string, std::string>> sorted;
    for (const auto& [path, sha] : files)
        if (GidHashesPath(path)) sorted.emplace_back(LowerSlashes(path), sha.empty() ? "-" : sha);
    std::sort(sorted.begin(), sorted.end());
    std::string t = "melange-gid/1\n";
    for (const auto& [path, sha] : sorted) t += "file=" + path + " " + sha + "\n";
    return t;
}

std::string BuildGidValue(const GameId& g) {
    if (!IsHex16(g.exe16) || !IsHex16(g.data16)) return "";
    return "1;" + g.exe16 + ";" + g.data16 + ";" + std::to_string(g.flags);
}

bool ParseGidValue(const std::string& value, GameId* out) {
    const std::vector<std::string> parts = Split(value, ';');
    if (parts.size() != 4 || parts[0] != "1" || !IsHex16(parts[1]) || !IsHex16(parts[2])) return false;
    // Digits only: strtoul alone would also take a sign or leading spaces ("-1" -> every flag set).
    if (parts[3].empty() || parts[3].size() > 9 ||
        !std::all_of(parts[3].begin(), parts[3].end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    const unsigned long flags = strtoul(parts[3].c_str(), nullptr, 10);
    if (out) *out = GameId{parts[1], parts[2], static_cast<uint32_t>(flags)};
    return true;
}

std::string GidFlagsText(uint32_t flags) {
    std::string s;
    if (flags & kGidData2) Add(s, "Data2 overlay");
    if (flags & kGidCrcOff) Add(s, "CRC check off");
    return s;
}

std::string DiffGid(const GameId& ours, const GameId& theirs) {
    std::string out;
    if (ours.exe16 != theirs.exe16) out = "different game build";
    const uint32_t differ = ours.flags ^ theirs.flags;
    if (ours.data16 != theirs.data16 || (differ & kGidData2)) {
        std::string data = "different game data files", who;
        if (theirs.flags & kGidData2) who = "theirs: Data2 overlay";
        if (ours.flags & kGidData2) who += (who.empty() ? "" : "; ") + std::string("yours: Data2 overlay");
        if (!who.empty()) data += " (" + who + ")";
        out += (out.empty() ? "" : "; ") + data;
    }
    if (differ & kGidCrcOff)
        out += (out.empty() ? "" : "; ") +
               std::string(theirs.flags & kGidCrcOff ? "their game's CRC check is off" : "your game's CRC check is off");
    return out;
}

std::vector<std::string> GidWarnings(const std::string& ourGid, const std::vector<GidMember>& members) {
    std::vector<std::string> out;
    GameId ours;
    if (!ParseGidValue(ourGid, &ours)) return out;
    for (const auto& m : members) {
        GameId theirs;
        if (!ParseGidValue(m.gid, &theirs)) continue;
        const std::string diff = DiffGid(ours, theirs);
        if (!diff.empty()) out.push_back((m.name.empty() ? std::string("?") : m.name) + ": " + diff);
    }
    return out;
}

}  // namespace melange::handshake
