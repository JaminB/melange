#include "mods/handshake_internal.h"

#include <algorithm>
#include <utility>
#include <cstdlib>

#include "tools/hash.h"

namespace melange::handshake {
namespace {
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
}  // namespace

std::string CanonicalText(const std::vector<ContentMod>& modsInLoadOrder, uint32_t modMessages) {
    std::string t = "melange-content/1\nsim=" + std::to_string(kSimApiVersion) + "\n";
    for (const auto& m : modsInLoadOrder) {
        t += "mod=" + m.id + "@" + m.version + "\n";
        for (const auto& f : m.files) t += "file=" + f.relPath + " " + f.sha256Hex + "\n";
    }
    t += "messages=" + std::to_string(modMessages) + "\n";
    return t;
}

std::string HashOfCanonicalText(const std::string& canonical) { return hashutil::Sha256Hex(canonical.data(), canonical.size()); }

mods::ContentId BuildContentId(std::vector<ContentMod> modsInLoadOrder, uint32_t modMessages) {
    for (auto& m : modsInLoadOrder)
        std::sort(m.files.begin(), m.files.end(), [](const ContentFile& a, const ContentFile& b) { return a.relPath < b.relPath; });
    mods::ContentId c{};
    c.contentMods = static_cast<uint32_t>(modsInLoadOrder.size());
    c.modMessages = modMessages;
    c.vanilla = modsInLoadOrder.empty() && modMessages == 0;
    if (!c.vanilla) {
        std::string hash = HashOfCanonicalText(CanonicalText(modsInLoadOrder, modMessages));
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
    std::string full;
    for (size_t i = 0; i < modsInLoadOrder.size(); ++i) {
        if (i) full += ',';
        full += modsInLoadOrder[i].id + "@" + modsInLoadOrder[i].version;
    }
    if (full.size() <= maxBytes) return full;
    const std::string ellipsis = "\xE2\x80\xA6";  // "…"
    std::string out;
    for (size_t i = 0; i < modsInLoadOrder.size(); ++i) {
        std::string piece = (i ? "," : "") + modsInLoadOrder[i].id + "@" + modsInLoadOrder[i].version;
        if (out.size() + piece.size() + ellipsis.size() > maxBytes) break;
        out += piece;
    }
    out += ellipsis;
    return out;
}

namespace {
std::vector<std::pair<std::string, std::string>> ParseModsValue(std::string v) {
    const std::string ellipsis = "\xE2\x80\xA6";
    if (v.size() >= ellipsis.size() && v.compare(v.size() - ellipsis.size(), ellipsis.size(), ellipsis) == 0)
        v.resize(v.size() - ellipsis.size());
    std::vector<std::pair<std::string, std::string>> out;
    size_t start = 0;
    while (start < v.size()) {
        size_t end = v.find(',', start);
        if (end == std::string::npos) end = v.size();
        std::string entry = v.substr(start, end - start);
        size_t at = entry.rfind('@');
        if (!entry.empty()) out.emplace_back(entry.substr(0, at), at == std::string::npos ? "" : entry.substr(at + 1));
        start = end + 1;
    }
    return out;
}
}  // namespace

std::string DiffModsValues(const std::string& ours, const std::string& theirs) {
    const auto a = ParseModsValue(ours), b = ParseModsValue(theirs);
    auto find = [](const auto& list, const std::string& id) -> const std::string* {
        for (const auto& [i, v] : list)
            if (i == id) return &v;
        return nullptr;
    };
    std::string missing, extra, versions;
    auto add = [](std::string& s, const std::string& item) { s += (s.empty() ? "" : ", ") + item; };
    for (const auto& [id, v] : a) {
        const std::string* t = find(b, id);
        if (!t) add(missing, id + "@" + v);
        else if (*t != v) add(versions, id + " " + v + " vs " + *t);
    }
    for (const auto& [id, v] : b)
        if (!find(a, id)) add(extra, id + "@" + v);
    std::string out;
    if (!missing.empty()) add(out, "missing " + missing);
    if (!extra.empty()) add(out, "extra " + extra);
    if (!versions.empty()) add(out, versions);
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

}  // namespace melange::handshake
