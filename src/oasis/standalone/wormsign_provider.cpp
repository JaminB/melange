#include "oasis/standalone/wormsign_provider.h"

#include <windows.h>

#include <algorithm>
#include <vector>

#include "tools/json_mini.h"
#include "tools/json_read.h"
#include "wormsign/format.h"
#include "wormsign/records.h"

namespace melange::oasis::standalone::wormsignprov {
namespace {
namespace wsr = melange::wormsign::wsr;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The ticks the file's TICK chunks hold, read from the chunks themselves so an incomplete file counts too.
uint32_t TickCount(const wsr::Reader& r) {
    uint32_t n = 0;
    r.ForEach(wsr::kTICK, [&](const wsr::ChunkRef&, const std::vector<uint8_t>& p) {
        melange::wormsign::rec::DecodeTicks(p.data(), p.size(), [&](const melange::wormsign::TickHash&) { ++n; });
    });
    return n;
}

bool HasChunk(const wsr::Reader& r, uint32_t type) {
    for (const wsr::ChunkRef& c : r.Chunks())
        if (c.type == type) return true;
    return false;
}

// HEAD's documented keys; anything else in there belongs to the recorder and is ignored here.
std::string EntryJson(const std::wstring& name, uint64_t bytes, const wsr::Reader& r) {
    json::Value head;
    json::Error e;
    (void)json::Parse(r.Header(), &head, &e);  // format.cpp's Reader::Parse already required this to be a JSON object
    auto str = [&](const char* key) -> std::string {
        const json::Value* m = head.Get(key);
        return m && m->IsString() ? m->string : "";
    };
    auto boolean = [&](const char* key) -> bool {
        const json::Value* m = head.Get(key);
        return m && m->IsBool() && m->boolean;
    };
    int64_t startUnix = 0;
    if (const json::Value* m = head.Get("startUnix"); m && m->IsNumber()) startUnix = static_cast<int64_t>(m->number);

    jsonmini::Obj o;
    o.Str("name", Narrow(name)).UInt("bytes", bytes).UInt("ticks", TickCount(r));
    o.Int("startUnix", startUnix).Str("exeBuild", str("exeBuild")).Str("melange", str("melange"));
    o.Str("land", str("land")).Str("contentHash", str("contentHash"));
    o.Bool("online", boolean("online")).Bool("complete", r.Complete());
    o.Bool("pinned", false);  // no live library to hold a pin, standalone
    o.Bool("flagged", HasChunk(r, wsr::kDVRG));
    return o.End();
}

struct FoundFile { std::wstring name; uint64_t bytes; };

std::vector<FoundFile> FindWsrFiles(const std::wstring& dir) {
    std::vector<FoundFile> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.wsr").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const uint64_t bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(FoundFile{fd.cFileName, bytes});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
}  // namespace

std::string ListJson(const std::wstring& replaysDir) {
    jsonmini::Arr arr;
    for (const FoundFile& f : FindWsrFiles(replaysDir)) {
        wsr::Reader r;
        std::string err;
        if (!r.OpenFile(replaysDir + L"\\" + f.name, &err)) continue;  // not a .wsr file at all: skip it
        arr.Raw(EntryJson(f.name, f.bytes, r));
    }
    return arr.End();
}
}  // namespace melange::oasis::standalone::wormsignprov
