// The speech banks on disk for wum.audio.voice. Parsing and decoding are fsb_voice.cpp; this file finds the file,
// reads the header once per bank and the data of one line on demand.
#include "audio/voice_banks.h"

#include <map>
#include <memory>

#include <windows.h>

#include "core/game.h"
#include "core/log.h"

namespace melange::audio::voicebanks {
namespace {
constexpr size_t kMaxCached = 64;  // there are 31 speech banks

struct Entry {
    std::wstring path;  // empty when the bank failed
    std::string why;
    std::unique_ptr<fsb::Bank> bank;
};
std::map<std::string, Entry> g_banks;  // by lower-case name

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::wstring Folder() { return game::GameDir() + L"\\Data\\Audio\\PC"; }

// The file whose name is `<lower>.fsb` in any case, or "".
std::wstring FindFile(const std::string& lower) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((Folder() + L"\\*.fsb").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    const std::string want = lower + ".fsb";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string n = Lower(game::Narrow(fd.cFileName));
        if (n == want) {
            found = Folder() + L"\\" + fd.cFileName;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

bool ReadAt(HANDLE f, uint64_t at, void* out, size_t n) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(at);
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    size_t done = 0;
    while (done < n) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(n - done > 0x100000 ? 0x100000 : n - done);
        if (!ReadFile(f, static_cast<uint8_t*>(out) + done, want, &got, nullptr) || got == 0) return false;
        done += got;
    }
    return true;
}

HANDLE Open(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

Entry Load(const std::string& lower) {
    Entry e;
    const std::wstring path = FindFile(lower);
    if (path.empty()) {
        e.why = "unknown bank";
        return e;
    }
    HANDLE f = Open(path);
    if (f == INVALID_HANDLE_VALUE) {
        e.why = "unreadable bank";
        return e;
    }
    LARGE_INTEGER size{};
    uint8_t head[fsb::kHeaderBytes];
    size_t need = 0;
    std::string why;
    std::vector<uint8_t> bytes;
    auto bank = std::make_unique<fsb::Bank>();
    const bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= static_cast<LONGLONG>(sizeof head) &&
                    ReadAt(f, 0, head, sizeof head) && fsb::HeaderBytes(head, sizeof head, &need, &why) &&
                    static_cast<uint64_t>(size.QuadPart) >= need && (bytes.resize(need), ReadAt(f, 0, bytes.data(), need)) &&
                    fsb::ParseBank(bytes.data(), bytes.size(), static_cast<uint64_t>(size.QuadPart), bank.get(), &why);
    CloseHandle(f);
    if (!ok) {
        LOG_WARN("[audio] speech bank %s unreadable: %s", lower.c_str(), why.empty() ? "read failed" : why.c_str());
        e.why = "unreadable bank";
        return e;
    }
    LOG_INFO("[audio] speech bank %s: %zu lines", lower.c_str(), bank->samples.size());
    e.path = path;
    e.bank = std::move(bank);
    return e;
}
}  // namespace

const fsb::Bank* Get(const std::string& bank, std::string* canonical, std::string* why) {
    if (!fsb::ValidBankName(bank)) {
        if (why) *why = "unknown bank";
        return nullptr;
    }
    const std::string lower = Lower(bank);
    auto it = g_banks.find(lower);
    if (it == g_banks.end()) {
        Entry e = Load(lower);
        // A bank that parsed is always kept (there are as many as files in the folder); failures only while there is
        // room, so asking for made-up names cannot grow the cache, it only costs a directory scan each time.
        if (!e.bank && g_banks.size() >= kMaxCached) {
            if (why) *why = e.why;
            return nullptr;
        }
        it = g_banks.emplace(lower, std::move(e)).first;
    }
    if (!it->second.bank) {
        if (why) *why = it->second.why;
        return nullptr;
    }
    if (canonical) *canonical = lower;
    return it->second.bank.get();
}

bool Decode(const std::string& canonical, const fsb::Sample& s, fsb::Pcm* out, std::string* why) {
    auto it = g_banks.find(canonical);
    if (it == g_banks.end() || !it->second.bank) {
        if (why) *why = "unknown bank";
        return false;
    }
    HANDLE f = Open(it->second.path);
    if (f == INVALID_HANDLE_VALUE) {
        if (why) *why = "unreadable bank";
        return false;
    }
    std::vector<uint8_t> data(s.bytes);
    const bool ok = s.bytes == 0 || ReadAt(f, s.offset, data.data(), data.size());
    CloseHandle(f);
    if (!ok) {
        if (why) *why = "unreadable bank";
        return false;
    }
    return fsb::DecodeSample(s, data.data(), data.size(), out, why);
}
}  // namespace melange::audio::voicebanks
