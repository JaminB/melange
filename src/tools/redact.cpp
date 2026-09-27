#include "tools/redact.h"

#include <cctype>
#include <regex>

#include "tools/hash.h"

namespace wf::redact {
namespace {
char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
}  // namespace

std::string RedactUserName(std::string_view text, std::string_view userName) {
    if (userName.size() < 3) return std::string(text);  // too short: more likely to false-positive than help
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (i + userName.size() <= text.size()) {
            bool match = true;
            for (size_t k = 0; k < userName.size() && match; ++k) match = Lower(text[i + k]) == Lower(userName[k]);
            if (match) {
                out += "%USERNAME%";
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
    static const std::regex kSteamId64(R"(\b7656119[0-9]{10}\b)");

    auto replaceAll = [&](std::string s, const std::regex& re) {
        std::string result;
        result.reserve(s.size());
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();
        size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            auto m = *it;
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
}  // namespace wf::redact
