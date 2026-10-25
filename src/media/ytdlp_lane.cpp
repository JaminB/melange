// The yt-dlp lane (pack_internal.h): yt-dlp breaks whenever YouTube changes, so it is not in the pinned pack but in its
// own folder Documents\Melange\media\ytdlp\<ver>\yt-dlp.exe, updated from the rolling manifest media-ytdlp.json that
// CI publishes on the JaminB/melange release media-ytdlp. Per the user's decision (design section 7) the manifest is
// unsigned: core checks the https prefix, the size and SHA-256 of the exe it names, and a serial that may never go
// down (no rollback to an older, exploitable yt-dlp). `yt-dlp -U` is never run.
#include <windows.h>

#include <vector>

#include "core/log.h"
#include "media/pack_internal.h"
#include "store/fetch.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::media::pack::core {
namespace {
std::wstring StatePath(const Layout& l) { return l.LaneRoot() + L"\\lane.json"; }

bool WriteState(const Layout& l, const LaneState& s) {
    jsonmini::Obj o;
    o.Str("version", s.lane.version)
        .Str("sha256", s.lane.sha256)
        .UInt("size", s.lane.size)
        .UInt("serial", s.lane.serial)
        .Int("lastCheck", s.lastCheck);
    return MakeDirs(l.LaneRoot()) && WriteFileAtomic(StatePath(l), o.End() + "\n");
}

uint64_t SizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return UINT64_MAX;
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

bool Fail(std::string* error, std::string* detail, const char* e, std::string d) {
    *error = e;
    *detail = std::move(d);
    return false;
}

// Older version folders of the lane; one in use (a running helper holds yt-dlp.exe) stays until the next update.
void DropOthers(const Layout& l, const std::string& keep) {
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileExW((l.LaneRoot() + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    if (fh == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> old;
    do {
        const std::string name = Narrow(fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && logic::ValidLaneVersion(name) && name != keep)
            old.push_back(l.LaneRoot() + L"\\" + fd.cFileName);
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    for (const std::wstring& d : old) DeleteTree(d);
}
}  // namespace

LaneState ReadLane(const Layout& l) {
    LaneState s;
    json::Value v;
    json::Error e;
    if (!json::ParseFile(StatePath(l), &v, &e, logic::kMaxManifestBytes) || !v.IsObject()) return s;
    const json::Value* ver = v.Get("version");
    const json::Value* sha = v.Get("sha256");
    const json::Value* size = v.Get("size");
    const json::Value* serial = v.Get("serial");
    const json::Value* last = v.Get("lastCheck");
    if (serial && serial->IsInteger() && serial->number > 0) s.lane.serial = static_cast<uint64_t>(serial->number);
    if (last && last->IsInteger()) s.lastCheck = static_cast<int64_t>(last->number);
    if (!ver || !ver->IsString() || !logic::ValidLaneVersion(ver->string) || !sha || !sha->IsString() ||
        !logic::ValidSha256Hex(sha->string) || !size || !size->IsInteger() || size->number <= 0)
        return s;  // keeps the serial (rollback protection) even without a usable exe
    s.lane.version = ver->string;
    s.lane.sha256 = sha->string;
    s.lane.size = static_cast<uint64_t>(size->number);
    const std::wstring exe = l.LaneRoot() + L"\\" + Widen(s.lane.version) + L"\\yt-dlp.exe";
    if (SizeOf(exe) == s.lane.size) s.exe = exe;
    return s;
}

bool UpdateLane(const Layout& l, const std::string& laneUrl, std::string_view prefix, const Hooks& h, LaneState* out,
                std::string* action, std::string* error, std::string* detail) {
    *action = "";
    const bool fileLane = logic::IsFileUrl(laneUrl);
    if (!logic::AllowedUrl(laneUrl, prefix, fileLane)) return Fail(error, detail, "manifest", "the lane URL is not allowed");
    store::fetch::StringSink sink;
    store::fetch::Options o;
    o.cap = logic::kMaxManifestBytes;
    o.totalMs = 30000;
    o.stallMs = 15000;
    o.cancel = h.cancel;
    o.registerActive = false;
    o.userAgent = h.userAgent;
    std::string why;
    if (!store::fetch::Get(laneUrl, sink, o, &why))
        return Fail(error, detail, why == "cancelled" ? "cancelled" : "network", "media-ytdlp.json: " + why);
    logic::Lane offer;
    if (!logic::ParseLane(sink.data, prefix, fileLane, &offer, &why)) return Fail(error, detail, "manifest", why);

    LaneState st = ReadLane(l);
    logic::LaneAction a = logic::DecideLane(st.lane, offer);
    // The recorded file is gone (deleted by hand, or a failed earlier install): fetch it again under the same serial.
    if (st.exe.empty() && (a == logic::LaneAction::Current || a == logic::LaneAction::Record)) a = logic::LaneAction::Install;
    *action = logic::LaneActionName(a);
    st.lastCheck = UnixNow();
    switch (a) {
        case logic::LaneAction::Rollback:
        case logic::LaneAction::Conflict:
            WriteState(l, st);  // a refused manifest still counts as a check: no retry storm
            *out = st;
            return Fail(error, detail, a == logic::LaneAction::Rollback ? "rollback" : "conflict",
                        "media-ytdlp.json serial " + std::to_string(offer.serial) + " (" + offer.version + ") against installed " +
                            std::to_string(st.lane.serial) + " (" + st.lane.version + ")");
        case logic::LaneAction::Current:
            WriteState(l, st);
            *out = st;
            return true;
        case logic::LaneAction::Record:
            st.lane.serial = offer.serial;
            WriteState(l, st);
            *out = ReadLane(l);
            return true;
        case logic::LaneAction::Install: break;
    }
    if (!MakeDirs(l.Dl()) || !MakeDirs(l.LaneRoot())) return Fail(error, detail, "disk", "cannot create " + Narrow(l.LaneRoot()));
    const std::wstring part = l.Dl() + L"\\ytdlp-" + Widen(offer.version) + L"-" + Widen(offer.sha256.substr(0, 8)) + L".exe.part";
    if (h.phase) h.phase("downloading");
    if (!FetchResumable(offer.url, part, offer.size, offer.sha256, h, error, detail)) return false;
    const std::wstring final_ = l.LaneRoot() + L"\\" + Widen(offer.version);
    const std::wstring tmp = final_ + L".tmp";
    DeleteTree(tmp);
    if (!CreateDirectoryW(tmp.c_str(), nullptr) ||
        !MoveFileExW(part.c_str(), (tmp + L"\\yt-dlp.exe").c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteTree(tmp);
        return Fail(error, detail, "disk", "cannot place yt-dlp.exe in " + Narrow(tmp));
    }
    if (GetFileAttributesW(final_.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteTree(final_)) {
        DeleteTree(tmp);
        return Fail(error, detail, "in-use", Narrow(final_) + " is in use");
    }
    if (!MoveFileExW(tmp.c_str(), final_.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteTree(tmp);
        return Fail(error, detail, "disk", "cannot rename " + Narrow(tmp));
    }
    st.lane = offer;
    if (!WriteState(l, st)) return Fail(error, detail, "disk", "cannot write lane.json");
    DropOthers(l, offer.version);
    *out = ReadLane(l);
    LOG_INFO("[media] yt-dlp %s installed (lane serial %llu)", offer.version.c_str(), offer.serial);
    return true;
}
}  // namespace melange::media::pack::core
