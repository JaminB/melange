#include "tools/redact.h"

#include <cctype>
#include <regex>

#include "tools/hash.h"

namespace melange::redact {
namespace {
char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
}  // namespace

std::string RedactUserName(std::string_view text, std::string_view userName) {
    return ReplaceName(text, userName, "%USERNAME%");
}

std::string ReplaceName(std::string_view text, std::string_view userName, std::string_view token) {
    if (userName.size() < 3) return std::string(text);  // too short: more likely to false-positive than help
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (i + userName.size() <= text.size()) {
            bool match = true;
            for (size_t k = 0; k < userName.size() && match; ++k) match = Lower(text[i + k]) == Lower(userName[k]);
            if (match) {
                out += token;
                i += userName.size();
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

std::string HashIdsAndIps(std::string_view text, std::string_view salt) {
    // std::regex works on std::string; the exporter only ever calls this on already-loaded, size-capped
    // text file contents (see export.h's 64 MB per-file cap), so a full copy here is not a hot path.
    std::string in(text);
    static const std::regex kIpv4(
        R"((?:(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\.){3}(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})(?::[0-9]{1,5})?)");
    // Any 64-bit SteamID printed in decimal is 17 digits, but the high bits (and so the leading digits) depend
    // on the account TYPE: individual accounts start "7656119...", but lobby/chat CSteamIDs (SteamTrace's Id(),
    // LocalNet's JoinLobby/LobbyEnter/LobbyChatUpdate logging) start "1097752...". Matching any 17-digit run
    // catches every SteamID64 variant instead of only individual accounts; a coincidental unrelated 17-digit
    // number is vanishingly unlikely to appear in these logs (see docs/m0-design.md SS3 "D" privacy note).
    static const std::regex kSteamId64(R"(\b[0-9]{17}\b)");

    // A match must be a whole token: not preceded by a digit or '.', and not followed by a digit or ".<digit>".
    // Otherwise version strings such as a GL driver's "26.8.1.260810" would be hashed as the IP "26.8.1.26".
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    auto wholeToken = [&](const std::string& s, size_t pos, size_t len) {
        if (pos > 0 && (isDigit(s[pos - 1]) || s[pos - 1] == '.')) return false;
        size_t e = pos + len;
        if (e < s.size() && isDigit(s[e])) return false;
        if (e + 1 < s.size() && s[e] == '.' && isDigit(s[e + 1])) return false;
        return true;
    };

    auto replaceAll = [&](std::string s, const std::regex& re) {
        std::string result;
        result.reserve(s.size());
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();
        size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            auto m = *it;
            if (!wholeToken(s, static_cast<size_t>(m.position()), static_cast<size_t>(m.length()))) continue;
            result.append(s, last, static_cast<size_t>(m.position()) - last);
            result += "hash:" + hashutil::ShortSaltedHash(salt, m.str());
            last = static_cast<size_t>(m.position() + m.length());
        }
        result.append(s, last, std::string::npos);
        return result;
    };

    in = replaceAll(std::move(in), kSteamId64);
    in = replaceAll(std::move(in), kIpv4);
    return in;
}
}  // namespace melange::redact
