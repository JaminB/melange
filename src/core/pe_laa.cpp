#include "core/pe_laa.h"

#include <cstring>
#include <vector>

#include "tools/hash.h"

namespace melange::pe {
namespace {
constexpr uint16_t kLaa = IMAGE_FILE_LARGE_ADDRESS_AWARE;
constexpr DWORD kHeadMax = 4096;   // the PE header must sit in the first 4 KB

// File offset of the Characteristics word, or 0 when the header is not a PE.
uint64_t CharsOffset(HANDLE f) {
    unsigned char head[kHeadMax];
    LARGE_INTEGER zero{};
    DWORD rd = 0;
    if (!SetFilePointerEx(f, zero, nullptr, FILE_BEGIN) || !ReadFile(f, head, sizeof head, &rd, nullptr) || rd < sizeof(IMAGE_DOS_HEADER))
        return 0;
    if (head[0] != 'M' || head[1] != 'Z') return 0;
    DWORD lfanew = 0;
    memcpy(&lfanew, head + offsetof(IMAGE_DOS_HEADER, e_lfanew), 4);
    if (lfanew < sizeof(IMAGE_DOS_HEADER) || lfanew + 4 + sizeof(IMAGE_FILE_HEADER) > rd) return 0;
    if (memcmp(head + lfanew, "PE\0\0", 4) != 0) return 0;
    return lfanew + 4 + offsetof(IMAGE_FILE_HEADER, Characteristics);   // e_lfanew + 22
}

HANDLE OpenFor(const std::wstring& path, DWORD access) {
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}
}  // namespace

bool ReadPeFlags(const std::wstring& path, uint16_t* characteristics, uint32_t* timestamp, uint32_t* checksum) {
    HANDLE f = OpenFor(path, GENERIC_READ);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    const uint64_t at = CharsOffset(f);
    if (at) {
        unsigned char head[kHeadMax];
        LARGE_INTEGER zero{};
        DWORD rd = 0;
        SetFilePointerEx(f, zero, nullptr, FILE_BEGIN);
        if (ReadFile(f, head, sizeof head, &rd, nullptr)) {
            const size_t nt = static_cast<size_t>(at) - 4 - offsetof(IMAGE_FILE_HEADER, Characteristics);
            const size_t optional = nt + 4 + sizeof(IMAGE_FILE_HEADER);
            // CheckSum is at the same offset (64) in the PE32 and PE32+ optional headers.
            if (optional + 68 <= rd) {
                ok = true;
                if (characteristics) memcpy(characteristics, head + at, 2);
                if (timestamp) memcpy(timestamp, head + nt + 4 + offsetof(IMAGE_FILE_HEADER, TimeDateStamp), 4);
                if (checksum) memcpy(checksum, head + optional + 64, 4);
            }
        }
    }
    CloseHandle(f);
    return ok;
}

std::string CanonicalSha256(HANDLE f) {
    const uint64_t at = CharsOffset(f);
    LARGE_INTEGER zero{};
    SetFilePointerEx(f, zero, nullptr, FILE_BEGIN);
    hashutil::Sha256 h;
    std::vector<unsigned char> buf(1 << 20);
    uint64_t pos = 0;
    DWORD rd = 0;
    while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &rd, nullptr) && rd) {
        // The Characteristics word is 2 bytes; the bit lives in the low byte (0x0020), at `at`.
        if (at && at >= pos && at < pos + rd) buf[static_cast<size_t>(at - pos)] &= static_cast<unsigned char>(~kLaa & 0xff);
        if (!h.Update(buf.data(), rd)) return {};
        pos += rd;
    }
    return h.FinishHex();
}

std::string CanonicalSha256(const std::wstring& path) {
    HANDLE f = OpenFor(path, GENERIC_READ);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s = CanonicalSha256(f);
    CloseHandle(f);
    return s;
}

bool IsLaaFile(const std::wstring& path) {
    uint16_t c = 0;
    return ReadPeFlags(path, &c) && (c & kLaa) != 0;
}

unsigned long SetLaaInFile(const std::wstring& path, bool on) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    unsigned long err = 0;
    const uint64_t at = CharsOffset(f);
    uint16_t c = 0;
    DWORD n = 0;
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(at);
    if (!at) {
        err = ERROR_BAD_EXE_FORMAT;
    } else if (!SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) || !ReadFile(f, &c, 2, &n, nullptr) || n != 2) {
        err = GetLastError() ? GetLastError() : ERROR_READ_FAULT;
    } else {
        const uint16_t want = on ? static_cast<uint16_t>(c | kLaa) : static_cast<uint16_t>(c & ~kLaa);
        if (want != c) {
            if (!SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) || !WriteFile(f, &want, 2, &n, nullptr) || n != 2 || !FlushFileBuffers(f))
                err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        }
    }
    CloseHandle(f);
    return err;
}
}  // namespace melange::pe
