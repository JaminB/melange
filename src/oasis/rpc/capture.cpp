// Oasis provider E: capture.list, capture.request, the `capture` channel and the /captures/<name>.mcap route
// (docs/capture-format.md). The viewer itself lives in web/src/panels/capture/.
#include "oasis/providers.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "melange/gltrace.h"
#include "melange/oasis.h"
#include "oasis/core/http.h"
#include "oasis/core/server.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::oasis::providers {
namespace {
namespace fs = std::filesystem;

constexpr int kErrRefused = -32000;
constexpr int kErrBadParams = -32602;

// Mirrors MirageTrace's private CaptureDir() (src/render/mirage/trace_capture.cpp). melange/gltrace.h does not
// expose the capture folder, so this reads the same config key and falls back to the same default instead of
// reaching into Mirage's internal header; if that default ever changes the two need updating together.
std::wstring CaptureDir() {
    const std::string configured = config::GetString("MirageTrace", "CaptureDir", "");
    if (!configured.empty()) return game::Widen(configured);
    PWSTR p = nullptr;
    std::wstring docs = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) docs = p;
    if (p) CoTaskMemFree(p);
    return docs + L"\\Melange\\captures";
}

bool ValidCaptureName(std::string_view name) {
    if (name.size() < 6 || name.size() > 128) return false;
    if (name.substr(name.size() - 5) != ".mcap") return false;
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) return false;
    return name.find("..") == std::string_view::npos;
}

ChannelId g_channel = 0;
std::mutex g_stateMx;
gltrace::CaptureState g_lastState = gltrace::CaptureState::Idle;

const char* StateName(gltrace::CaptureState s) {
    switch (s) {
        case gltrace::CaptureState::Idle: return "idle";
        case gltrace::CaptureState::Armed: return "armed";
        case gltrace::CaptureState::Recording: return "recording";
        case gltrace::CaptureState::Writing: return "writing";
        case gltrace::CaptureState::Done: return "done";
        case gltrace::CaptureState::Failed: return "failed";
    }
    return "idle";
}

std::string StateJson() {
    std::wstring path;
    std::string error;
    const gltrace::CaptureState st = gltrace::CaptureStatus(&path, &error);
    jsonmini::Obj o;
    o.Str("state", StateName(st));
    o.Str("path", path.empty() ? "" : game::Narrow(path));
    if (!error.empty()) o.Str("error", error);
    return o.End();
}

void OnFrame() {
    std::wstring path;
    std::string error;
    const gltrace::CaptureState st = gltrace::CaptureStatus(&path, &error);
    std::lock_guard lk(g_stateMx);
    if (st == g_lastState) return;
    g_lastState = st;
    if (g_channel && HasSubscribers(g_channel)) Publish(g_channel, StateJson());
}

void OnCaptureSubscribe(ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (subscribed) PublishTo(ch, client, StateJson());
}

void RpcList(const Call&, Result& r, void*) {
    jsonmini::Arr arr;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(CaptureDir(), ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::string name = game::Narrow(entry.path().filename().wstring());
        if (!ValidCaptureName(name)) continue;
        std::error_code sizeEc, timeEc;
        const uint64_t bytes = static_cast<uint64_t>(entry.file_size(sizeEc));
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(entry.last_write_time(timeEc));
        const uint64_t ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count());
        arr.Raw(jsonmini::Obj().Str("name", name).UInt("bytes", sizeEc ? 0 : bytes).UInt("time", timeEc ? 0 : ms).End());
    }
    r.json = arr.End();
}

void RpcRequest(const Call& c, Result& r, void*) {
    json::Value v;
    json::Error e;
    const std::string_view p = c.paramsJson.empty() ? std::string_view("{}") : c.paramsJson;
    if (!json::Parse(p, &v, &e) || !(v.IsObject() || v.IsNull())) {
        r.ok = false;
        r.code = kErrBadParams;
        r.message = "params must be an object";
        return;
    }
    gltrace::CaptureOptions opt;
    if (const auto* m = v.Get("frames"); m && m->IsNumber()) opt.frames = static_cast<uint32_t>(m->number);
    if (const auto* m = v.Get("textures"); m && m->IsBool()) opt.textures = m->boolean;
    if (const auto* m = v.Get("shaders"); m && m->IsBool()) opt.shaders = m->boolean;
    if (const auto* m = v.Get("frameImage"); m && m->IsBool()) opt.frameImage = m->boolean;
    if (const auto* m = v.Get("bufferSizes"); m && m->IsBool()) opt.bufferSizes = m->boolean;
    if (const auto* m = v.Get("maxTextureMB"); m && m->IsNumber()) opt.maxTextureMB = static_cast<uint32_t>(m->number);
    if (!gltrace::RequestCapture(opt)) {
        r.ok = false;
        r.code = kErrRefused;
        r.message = gltrace::Installed() ? "a capture is already running" : "needs [MirageTrace] Mode=count or log";
        return;
    }
    // Published right away: MirageTrace's own Frame handler can advance Armed -> Recording before OnFrame's next
    // poll runs, so waiting for the poll to notice the change could skip "armed" entirely.
    {
        std::lock_guard lk(g_stateMx);
        g_lastState = gltrace::CaptureState::Armed;
    }
    if (g_channel && HasSubscribers(g_channel)) Publish(g_channel, StateJson());
    r.json = jsonmini::Obj().Bool("armed", true).End();
}

bool RouteCaptures(const core::Request& rq, core::Response* out, void*) {
    constexpr size_t kPrefixLen = 10;  // "/captures/"
    const std::string name = std::string(rq.path).substr(kPrefixLen);
    if (!ValidCaptureName(name)) {
        out->status = 404;
        out->body = "Not Found\n";
        return true;
    }
    const std::wstring full = CaptureDir() + L"\\" + game::Widen(name);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec) || ec) {
        out->status = 404;
        out->body = "Not Found\n";
        return true;
    }
    out->contentType = "application/octet-stream";
    out->file = full;
    return true;
}
}  // namespace

void InstallCapture() {
    g_channel = AddChannel("capture", ChannelOptions{Overflow::Coalesce});
    OnSubscribe(g_channel, &OnCaptureSubscribe, nullptr);
    AddMethod("capture.list", &RpcList, nullptr, kRpcServerThread);
    AddMethod("capture.request", &RpcRequest, nullptr, kRpcMutating | kRpcGameOnly);
    core::AddRoute("/captures/", &RouteCaptures, nullptr);
    events::Subscribe(events::Event::Frame, &OnFrame);
}
}  // namespace melange::oasis::providers
