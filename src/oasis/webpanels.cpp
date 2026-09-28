// Web panels from C++ modules and Lua mods (wum.web.panel): protocol registration (melange/oasis.h) plus the
// /ext/<id>/ route, sandboxed with its own CSP so a panel iframe has no cookie and no same-origin fetch to /ws.
#include "oasis/webpanels.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "melange/oasis.h"
#include "oasis/core/files.h"
#include "oasis/core/http.h"
#include "oasis/core/server.h"
#include "oasis/providers.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
struct Panel {
    int webHandle = 0, routeHandle = 0;
    std::string id, title, entry, url;
    std::unique_ptr<core::Files> files;
};

std::mutex g_mx;
std::vector<std::unique_ptr<Panel>> g_panels;

bool ServeExt(const core::Request& rq, core::Response* out, void* user) {
    auto* p = static_cast<Panel*>(user);
    const std::string prefix = "/ext/" + p->id + "/";
    std::string rel = rq.path.size() > prefix.size() ? rq.path.substr(prefix.size()) : "";
    if (rel.empty()) rel = p->entry;
    if (!core::SafePath(rel)) return false;
    bool gz = false;
    std::string etag;
    if (!p->files->Get(rel, &out->body, &etag, &gz)) return false;
    out->status = 200;
    out->contentType = core::MimeType(rel);
    out->cacheable = false;
    out->headers.emplace_back("Content-Security-Policy", "sandbox allow-scripts");
    return true;
}

void WebPanels(const Call&, Result& r, void*) {
    std::lock_guard lk(g_mx);
    jsonmini::Arr a;
    for (const auto& p : g_panels) a.Raw(jsonmini::Obj().Str("id", p->id).Str("title", p->title).Str("url", p->url).End());
    r.json = a.End();
}
}  // namespace

int AddPanel(const char* id, const char* title, const wchar_t* dir, const char* entry) {
    if (!id || !title || !dir || !entry) return 0;
    const int h = melange::oasis::AddWebPanel(id, title, dir, entry);
    if (!h) return 0;
    auto p = std::make_unique<Panel>();
    p->webHandle = h;
    p->id = id;
    p->title = title;
    p->entry = entry;
    p->url = "/ext/" + p->id + "/" + p->entry;
    p->files = core::DirFiles(dir);
    const std::string prefix = "/ext/" + p->id + "/";
    p->routeHandle = core::AddRoute(prefix.c_str(), &ServeExt, p.get());
    std::lock_guard lk(g_mx);
    g_panels.push_back(std::move(p));
    return h;
}

void RemovePanel(int handle) {
    if (!handle) return;
    std::lock_guard lk(g_mx);
    for (auto it = g_panels.begin(); it != g_panels.end(); ++it) {
        if ((*it)->webHandle != handle) continue;
        core::RemoveRoute((*it)->routeHandle);
        melange::oasis::RemoveWebPanel(handle);
        g_panels.erase(it);
        return;
    }
}

void InstallWebPanels() { melange::oasis::AddMethod("web.panels", &WebPanels, nullptr, kRpcServerThread); }
}  // namespace melange::oasis::providers
