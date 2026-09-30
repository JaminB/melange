// The /erg/assets/<key>.glb|.png route. Conversion itself lives in erg/preview.*; this file only wires the
// game's Data directory into it and serves the resulting bytes.
#include "oasis/providers.h"

#include <string>
#include <string_view>

#include "erg/preview.h"
#include "oasis/core/http.h"
#include "oasis/core/server.h"

namespace melange::oasis::providers {
namespace {

constexpr std::string_view kPrefix = "/erg/assets/";

bool RouteErgAssets(const core::Request& rq, core::Response* out, void*) {
    const std::string_view rest(rq.path.data() + kPrefix.size(), rq.path.size() - kPrefix.size());
    const size_t dot = rest.find_last_of('.');
    if (dot == std::string_view::npos) {
        out->status = 404;
        out->body = "Not Found\n";
        return true;
    }
    const std::string key(rest.substr(0, dot));
    const std::string ext(rest.substr(dot + 1));

    erg::preview::Asset asset;
    std::string err;
    if (!erg::preview::Get(key, ext, &asset, &err)) {
        out->status = 404;
        out->body = "Not Found\n";
        return true;
    }
    out->status = 200;
    out->contentType = asset.contentType;
    out->body.assign(reinterpret_cast<const char*>(asset.bytes.data()), asset.bytes.size());
    out->headers.emplace_back("ETag", asset.etag);
    out->headers.emplace_back("Cache-Control", "private, max-age=86400");  // Head() skips its own default for this
    return true;
}

}  // namespace

void InstallErgAssetRoute(const std::wstring& gameDir, int cacheMB) {
    erg::preview::SetGameDir(gameDir);
    erg::preview::SetCacheLimitMB(cacheMB);
    core::AddRoute("/erg/assets/", &RouteErgAssets, nullptr);  // must be a literal: AddRoute keeps the pointer
}
}  // namespace melange::oasis::providers
