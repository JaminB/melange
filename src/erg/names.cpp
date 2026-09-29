#include "erg/names.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace melange::erg::names {
namespace {
constexpr std::string_view kVanilla[] = {
#include "erg/vanilla_stems.inc"
};

char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool LessI(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return Lower(x) < Lower(y); });
}

bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}
}  // namespace

std::string Prefix(std::string_view modId) {
    std::string p(modId);
    std::replace(p.begin(), p.end(), '-', '_');
    return p;
}

bool ValidSlug(std::string_view slug) {
    if (slug.empty() || slug.size() > 24) return false;
    return std::all_of(slug.begin(), slug.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

bool ValidPrefix(std::string_view prefix, std::string* err) {
    if (prefix.empty() || prefix.size() > kMaxStem - 2) return Fail(err, "the prefix must be 1-46 characters");
    for (char c : prefix)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return Fail(err, "the prefix '" + std::string(prefix) + "' may hold only a-z, 0-9 and '_'");
    if (prefix == kTestPrefix) return Fail(err, "the prefix 'ergtest' is reserved for Erg Test levels");
    return true;
}

bool ValidStem(std::string_view stem, std::string_view prefix, std::string* err) {
    if (stem.find('.') != std::string_view::npos) return Fail(err, "a level stem cannot contain '.'");
    if (stem.size() > kMaxStem) return Fail(err, "a level stem is at most 48 characters");
    if (prefix.empty() || stem.size() <= prefix.size() + 1 || stem.substr(0, prefix.size()) != prefix ||
        stem[prefix.size()] != '_')
        return Fail(err, "the stem '" + std::string(stem) + "' must start with '" + std::string(prefix) + "_'");
    if (prefix != kTestPrefix) {
        std::string why;
        if (!ValidPrefix(prefix, &why)) return Fail(err, why);
    }
    const std::string_view slug = stem.substr(prefix.size() + 1);
    if (!ValidSlug(slug)) return Fail(err, "the slug '" + std::string(slug) + "' must match [a-z0-9]{1,24}");
    if (CollidesWithVanilla(stem)) return Fail(err, "the stem '" + std::string(stem) + "' is a vanilla level stem");
    return true;
}

bool CollidesWithVanilla(std::string_view stem) {
    const auto it = std::lower_bound(std::begin(kVanilla), std::end(kVanilla), stem, LessI);
    return it != std::end(kVanilla) && !LessI(stem, *it) && !LessI(*it, stem);
}

std::string Key(std::string_view stem) { return "Multi." + std::string(stem); }
}  // namespace melange::erg::names
