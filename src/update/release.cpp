#include "update/release.h"

#include <algorithm>

#include "tools/json_read.h"

namespace melange::update {
namespace {
std::string Str(const json::Value& o, const char* k) {
    const json::Value* v = o.Get(k);
    return v && v->IsString() ? v->string : std::string();
}

bool Hex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool Fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}
}  // namespace

int CompareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v) {
        std::vector<long> out;
        long cur = 0;
        bool any = false;
        for (char ch : v) {
            if (ch >= '0' && ch <= '9') {
                cur = cur * 10 + (ch - '0');
                any = true;
            } else if (ch == '.') {
                out.push_back(cur);
                cur = 0;
                any = false;
            } else {
                break;
            }
        }
        if (any || !out.empty()) out.push_back(cur);
        while (out.size() > 1 && out.back() == 0) out.pop_back();
        return out;
    };
    const auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

bool PlainVersion(std::string_view v) {
    if (v.empty() || v.size() > 23) return false;
    int parts = 1, digits = 0;
    for (char c : v) {
        if (c == '.') {
            if (!digits) return false;
            ++parts;
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 5) return false;
        } else {
            return false;
        }
    }
    return digits > 0 && parts <= 4;
}

const Asset* Release::Find(std::string_view name) const {
    for (const auto& a : assets)
        if (a.name == name) return &a;
    return nullptr;
}

bool ParseRelease(std::string_view text, Release* out, std::string* err) {
    *out = Release{};
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e, kMaxApiBytes)) return Fail(err, "the release answer is not JSON (" + e.text + ")");
    if (!v.IsObject()) return Fail(err, "the release answer is not an object");
    for (const char* k : {"draft", "prerelease"})
        if (const json::Value* b = v.Get(k); b && b->IsBool() && b->boolean) return Fail(err, std::string("the release is a ") + k);
    out->tag = Str(v, "tag_name");
    if (out->tag.size() < 2 || out->tag[0] != 'v' || !PlainVersion(std::string_view(out->tag).substr(1)))
        return Fail(err, "the release tag '" + out->tag.substr(0, 40) + "' is not v<version>");
    out->version = out->tag.substr(1);
    out->htmlUrl = Str(v, "html_url");
    if (out->htmlUrl.rfind("https://", 0) != 0) out->htmlUrl.clear();
    if (const json::Value* a = v.Get("assets"); a && a->IsArray())
        for (const auto& it : a->items) {
            if (!it.IsObject()) continue;
            Asset as{Str(it, "name"), Str(it, "browser_download_url")};
            if (const json::Value* n = it.Get("size"); n && n->IsInteger() && n->number >= 0) as.size = static_cast<uint64_t>(n->number);
            if (!as.name.empty() && !as.url.empty()) out->assets.push_back(std::move(as));
        }
    return true;
}

bool ParseManifest(std::string_view text, const std::string& expectVersion, Manifest* out, std::string* err) {
    *out = Manifest{};
    json::Value v;
    json::Error e;
    if (text.size() > kMaxManifestBytes) return Fail(err, "the release manifest is too large");
    if (!json::Parse(text, &v, &e) || !v.IsObject()) return Fail(err, "the release manifest is not a JSON object");
    out->version = Str(v, "version");
    out->zip = Str(v, "zip");
    out->sha256 = Str(v, "sha256");
    std::transform(out->sha256.begin(), out->sha256.end(), out->sha256.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'F' ? c + 32 : c); });
    const json::Value* size = v.Get("size");
    if (!PlainVersion(out->version) || out->version != expectVersion)
        return Fail(err, "the release manifest is for version '" + out->version.substr(0, 40) + "', expected " + expectVersion);
    if (out->zip != "melange-" + expectVersion + ".zip") return Fail(err, "the release manifest names an unexpected zip '" + out->zip.substr(0, 80) + "'");
    if (!Hex64(out->sha256)) return Fail(err, "the release manifest has no valid sha256");
    if (!size || !size->IsInteger() || size->number <= 0 || size->number > static_cast<double>(kMaxZipBytes))
        return Fail(err, "the release manifest has no valid size");
    out->size = static_cast<uint64_t>(size->number);
    return true;
}

bool AllowedUrl(std::string_view url, std::string_view prefix) {
    if (prefix.empty() || url.size() <= prefix.size() || url.substr(0, prefix.size()) != prefix) return false;
    const std::string_view rest = url.substr(prefix.size());
    if (rest.find("..") != std::string_view::npos || rest.find('\\') != std::string_view::npos) return false;
    for (char c : rest)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '?' || c == '#' || c == '%' || c == '@') return false;
    return true;
}
}  // namespace melange::update
