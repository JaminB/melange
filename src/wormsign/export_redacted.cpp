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
}  // namespace

bool ExportRedacted(const std::wstring& path, const std::wstring& outPath, std::string* error, const std::string& saltIn) {
    namespace wsr = melange::wormsign::wsr;
    wsr::Reader r;
    std::string err;
    if (!r.OpenFile(path, &err)) {
        if (error) *error = "could not open source: " + err;
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
