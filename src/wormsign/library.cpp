// The replay library. Indexes *.wsr files in <Documents>\Melange\replays, enforces retention (KeepMatches /
// MaxMB, pinned and flagged exempt), and defines the public melange::wormsign::Library()/Pin().
#include "wormsign/library.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/log.h"
#include "tools/hash.h"
#include "tools/json_read.h"
#include "tools/redact.h"
#include "wormsign/format.h"
#include "wormsign/records.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace melange::wormsign::library {
namespace wsr = melange::wormsign::wsr;
namespace rec = melange::wormsign::rec;

struct Entry {
    std::wstring path;
    ReplayInfo info{};
    bool pinned = false;
    FILETIME mtime{};
};

// Not in an unnamed namespace: melange::wormsign::Library()/Pin() (declared outside this namespace, directly in
// melange/wormsign.h) need to reach these by qualified name.
std::mutex g_mu;
std::vector<Entry> g_entries;  // newest first, g_mu held
int g_keepMatches = 20;
uint32_t g_maxMB = 200;
bool g_configured = false;

// Also outside the unnamed namespace: Pin() (in melange::wormsign) builds the same sidecar path by qualified
// name, library::PinnedMarker.
std::wstring PinnedMarker(const std::wstring& wsrPath) { return wsrPath + L".pinned"; }

namespace {
void CopyStr(char* dst, size_t cap, const std::string& s) {
    const size_t n = (std::min)(s.size(), cap - 1);
    memcpy(dst, s.data(), n);
    dst[n] = 0;
}
void CopyPath(wchar_t* dst, size_t cap, const std::wstring& s) {
    const size_t n = (std::min)(s.size(), cap - 1);
    memcpy(dst, s.data(), n * sizeof(wchar_t));
    dst[n] = 0;
}

bool HasPinnedMarker(const std::wstring& wsrPath) {
    return GetFileAttributesW(PinnedMarker(wsrPath).c_str()) != INVALID_FILE_ATTRIBUTES;
}

int64_t CountRecords(const wsr::Reader& r, uint32_t type, bool remote) {
    int64_t total = 0;
    bool bad = false;
    std::vector<rec::Input> inputs;
    r.ForEach(type, [&](const wsr::ChunkRef&, const std::vector<uint8_t>& p) {
        if (remote) {
            const int64_t n = rec::ForEachRemoteInput(p.data(), p.size(), [](uint32_t, uint16_t, uint32_t, uint32_t) {});
            if (n < 0) bad = true;
            else total += n;
            return;
        }
        inputs.clear();
        if (!rec::DecodeInputs(p.data(), p.size(), &inputs)) bad = true;
        total += static_cast<int64_t>(inputs.size());
    });
    return bad ? -1 : total;
}

uint32_t CountTicks(const wsr::Reader& r) {
    uint32_t total = 0;
    r.ForEach(wsr::kTICK, [&](const wsr::ChunkRef&, const std::vector<uint8_t>& p) {
        rec::DecodeTicks(p.data(), p.size(), [&](const TickHash&) { ++total; });
    });
    return total;
}

bool AnyChunk(const wsr::Reader& r, uint32_t type) {
    for (const auto& c : r.Chunks())
        if (c.type == type) return true;
    return false;
}

std::string FirstPayload(const wsr::Reader& r, uint32_t type) {
    std::string out;
    r.ForEach(type, [&](const wsr::ChunkRef&, const std::vector<uint8_t>& p) {
        if (out.empty()) out.assign(p.begin(), p.end());
    });
    return out;
}

bool BuildEntry(const std::wstring& path, const WIN32_FIND_DATAW& fd, Entry* out) {
    wsr::Reader r;
    std::string err;
    if (!r.OpenFile(path, &err)) return false;

    ReplayInfo info{};
    CopyPath(info.path, 260, path);
    info.bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    info.complete = r.Complete();
    info.flagged = AnyChunk(r, wsr::kDVRG);
    info.ticks = CountTicks(r);
    const int64_t in = CountRecords(r, wsr::kINPT, false);
    const int64_t rin = CountRecords(r, wsr::kRMTI, true);
    info.inputs = in > 0 ? static_cast<uint32_t>(in) : 0;
    info.remoteInputs = rin > 0 ? static_cast<uint32_t>(rin) : 0;

    melange::json::Value head;
    melange::json::Error jerr;
    if (melange::json::Parse(r.Header(), &head, &jerr)) {
        if (const auto* v = head.Get("exeBuild"); v && v->IsString()) CopyStr(info.exeBuild, sizeof info.exeBuild, v->string);
        if (const auto* v = head.Get("melange"); v && v->IsString()) CopyStr(info.melange, sizeof info.melange, v->string);
        if (const auto* v = head.Get("contentHash"); v && v->IsString())
            CopyStr(info.contentHash16, sizeof info.contentHash16, v->string);
        if (const auto* v = head.Get("startUnix"); v && v->IsNumber()) info.startUnix = static_cast<int64_t>(v->number);
        if (const auto* v = head.Get("online"); v && v->IsBool()) info.online = v->boolean;
    }
    melange::json::Value setp;
    if (melange::json::Parse(FirstPayload(r, wsr::kSETP), &setp, &jerr)) {
        if (const auto* v = setp.Get("landFile"); v && v->IsString()) CopyStr(info.land, sizeof info.land, v->string);
    }

    out->path = path;
    out->info = info;
    out->pinned = HasPinnedMarker(path);
    out->info.pinned = out->pinned;
    out->mtime = fd.ftLastWriteTime;
    return true;
}

// Deletes the oldest files once past KeepMatches or MaxMB; pinned and flagged files are never counted or deleted.
// Called with g_mu held.
void EnforceRetentionLocked() {
    std::vector<size_t> idx(g_entries.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(),
              [](size_t a, size_t b) { return CompareFileTime(&g_entries[a].mtime, &g_entries[b].mtime) > 0; });

    std::vector<size_t> candidates;  // unpinned, unflagged, newest first
    uint64_t totalBytes = 0;
    for (size_t i : idx) {
        if (g_entries[i].pinned || g_entries[i].info.flagged) continue;
        candidates.push_back(i);
        totalBytes += g_entries[i].info.bytes;
    }
    const uint64_t capBytes = static_cast<uint64_t>(g_maxMB) * 1024 * 1024;

    std::vector<bool> toDelete(g_entries.size(), false);
    size_t kept = 0;
    for (size_t i : candidates) {
        const bool overCount = kept >= static_cast<size_t>(g_keepMatches);
        const bool overBytes = totalBytes > capBytes;
        if (overCount || overBytes) {
            toDelete[i] = true;
            totalBytes -= g_entries[i].info.bytes;
        } else {
            ++kept;
        }
    }
    std::vector<Entry> survivors;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        if (toDelete[i]) {
            if (!DeleteFileW(g_entries[i].path.c_str()))
                LOG_WARN("[wormsign] library: could not delete %ls (retention)", g_entries[i].path.c_str());
            else
                LOG_INFO("[wormsign] library: pruned %ls (retention)", g_entries[i].path.c_str());
        } else {
            survivors.push_back(std::move(g_entries[i]));
        }
    }
    g_entries.swap(survivors);
}
}  // namespace

std::wstring g_testDir;

std::wstring ReplaysDir() {
    if (!g_testDir.empty()) {
        SHCreateDirectoryExW(nullptr, g_testDir.c_str(), nullptr);
        return g_testDir;
    }
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = docs;
    if (docs) CoTaskMemFree(docs);
    if (out.empty()) return L"";
    out += L"\\Melange\\replays";
    SHCreateDirectoryExW(nullptr, out.c_str(), nullptr);
    return out;
}

void SetReplaysDirForTests(const std::wstring& dir) { g_testDir = dir; }

void Configure(int keepMatches, uint32_t maxMB) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_keepMatches = (std::max)(1, keepMatches);
    g_maxMB = (std::max)(1u, maxMB);
    g_configured = true;
    EnforceRetentionLocked();
}

void Rescan() {
    const std::wstring dir = ReplaysDir();
    if (dir.empty()) return;
    std::vector<Entry> found;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\wsr-*.wsr").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Entry e;
            if (BuildEntry(dir + L"\\" + fd.cFileName, fd, &e)) found.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(),
              [](const Entry& a, const Entry& b) { return CompareFileTime(&a.mtime, &b.mtime) > 0; });
    std::lock_guard<std::mutex> lk(g_mu);
    g_entries.swap(found);
    if (g_configured) EnforceRetentionLocked();
}

void OnRecordingClosed(const std::wstring& path, bool complete) {
    (void)complete;  // the reader re-derives completeness from the trailer, which is the ground truth
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(path.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FindClose(h);
    Entry e;
    if (!BuildEntry(path, fd, &e)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_entries.insert(g_entries.begin(), std::move(e));
    EnforceRetentionLocked();
}

bool ExportRedacted(const std::wstring& path, const std::wstring& outPath, std::string* error, const std::string& saltIn) {
    wsr::Reader r;
    std::string err;
    if (!r.OpenFile(path, &err)) {
        if (error) *error = "could not open source: " + err;
        return false;
    }
    wchar_t userBuf[256];
    DWORD userLen = 256;
    const std::string userName = GetUserNameW(userBuf, &userLen) ? melange::game::Narrow(userBuf) : std::string();
    const std::string salt = saltIn.empty() ? melange::hashutil::RandomSalt() : saltIn;

    wsr::Writer w;
    if (!w.Open(outPath)) {
        if (error) *error = "could not create " + melange::game::Narrow(outPath);
        return false;
    }
    bool ok = true;
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> ticks;
    for (const auto& e : r.Index()) ticks[e.offset] = {e.tickFrom, e.tickTo};
    for (const auto& c : r.Chunks()) {
        if (c.type == wsr::kINDX) continue;
        std::vector<uint8_t> payload;
        if (!r.Payload(c, &payload)) {
            ok = false;
            continue;
        }
        // Only the JSON chunks can carry a SteamID, IP or a path with the Windows user name in it; every other
        // chunk type is a fixed binary record shape with no identity fields.
        if (c.type == wsr::kHEAD || c.type == wsr::kSETP || c.type == wsr::kNOTE || c.type == wsr::kDVRG ||
            c.type == wsr::kENGV) {
            std::string text(payload.begin(), payload.end());
            text = melange::redact::HashIdsAndIps(text, salt);
            if (!userName.empty()) text = melange::redact::RedactUserName(text, userName);
            payload.assign(text.begin(), text.end());
        }
        const auto t = ticks.find(c.offset);
        const uint32_t from = t == ticks.end() ? 0 : t->second.first, to = t == ticks.end() ? 0 : t->second.second;
        if (!w.Chunk(c.type, payload.data(), payload.size(), true, from, to)) ok = false;
    }
    const bool closed = w.Close();
    if (!closed || !ok) {
        if (error) *error = "failed while rewriting chunks";
        return false;
    }
    return true;
}
}  // namespace melange::wormsign::library

namespace melange::wormsign {
int Library(ReplayInfo* out, int max) {
    std::lock_guard<std::mutex> lk(library::g_mu);
    const int n = (std::min)(max, static_cast<int>(library::g_entries.size()));
    for (int i = 0; i < n; ++i) out[i] = library::g_entries[static_cast<size_t>(i)].info;
    return static_cast<int>(library::g_entries.size());
}

bool Pin(const wchar_t* path, bool pinned) {
    if (!path) return false;
    std::lock_guard<std::mutex> lk(library::g_mu);
    for (auto& e : library::g_entries) {
        if (e.path != path) continue;
        const std::wstring marker = library::PinnedMarker(e.path);
        bool ok;
        if (pinned) {
            HANDLE f = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            ok = f != INVALID_HANDLE_VALUE;
            if (ok) CloseHandle(f);
        } else {
            ok = DeleteFileW(marker.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
        }
        if (ok) {
            e.pinned = pinned;
            e.info.pinned = pinned;
        }
        return ok;
    }
    return false;
}
}  // namespace melange::wormsign
