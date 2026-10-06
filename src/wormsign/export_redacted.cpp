// library::ExportRedacted: the shareable copy of a .wsr recording. Kept apart from the library (retention, the
// index) because it needs nothing from the game, so Melange.exe's log export can link it too.
#include "wormsign/library.h"

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "tools/hash.h"
#include "tools/redact.h"
#include "wormsign/format.h"

namespace melange::wormsign::library {
namespace {
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

uint64_t SizeOf(const std::wstring& path, bool* exists) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    *exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) != 0;
    return *exists ? (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow : 0;
}
}  // namespace

bool ExportRedacted(const std::wstring& path, const std::wstring& outPath, std::string* error, const std::string& saltIn,
                    bool* incomplete) {
    namespace wsr = melange::wormsign::wsr;
    if (incomplete) *incomplete = false;
    bool exists = false;
    const uint64_t size = SizeOf(path, &exists);
    if (exists && size == 0) {
        if (error) *error = "empty: the game crashed before the recording was written";
        return false;
    }
    wsr::Reader r;
    std::string err;
    if (!r.OpenFile(path, &err)) {
        // A file cut off inside its first chunk: the recording had only just started.
        if (err == "no HEAD chunk" || (err == "not a .wsr file" && size < wsr::kMagicBytes))
            err = "no complete header (" + std::to_string(size) + " bytes): the game stopped before the recording's "
                  "first chunk was written";
        if (error) *error = err;
        return false;
    }
    wchar_t userBuf[256];
    DWORD userLen = 256;
    const std::string userName = GetUserNameW(userBuf, &userLen) ? Utf8(userBuf) : std::string();
    const std::string salt = saltIn.empty() ? melange::hashutil::RandomSalt() : saltIn;

    wsr::Writer w;
    if (!w.Open(outPath)) {
        if (error) *error = "could not create " + Utf8(outPath);
        return false;
    }
    // A recording cut off by a crash (no INDX/trailer) is copied as far as it reads, and stays without INDX so the copy
    // reads as incomplete too. Nothing past the first chunk that does not decode is copied: it could not be redacted.
    bool ok = true, cut = !r.Complete();
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> ticks;
    for (const auto& e : r.Index()) ticks[e.offset] = {e.tickFrom, e.tickTo};
    for (const auto& c : r.Chunks()) {
        if (c.type == wsr::kINDX) continue;
        std::vector<uint8_t> payload;
        if (!r.Payload(c, &payload)) {
            cut = true;
            break;
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
    bool closed;
    if (cut) {
        closed = w.Flush();
        w.Abandon();
    } else {
        closed = w.Close();
    }
    if (!closed || !ok) {
        if (error) *error = "failed while rewriting chunks";
        return false;
    }
    if (incomplete) *incomplete = cut;
    return true;
}
}  // namespace melange::wormsign::library
