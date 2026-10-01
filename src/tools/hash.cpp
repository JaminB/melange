#include "tools/hash.h"

#include <windows.h>

#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace melange::hashutil {
namespace {
std::string HexOf(const unsigned char* dig, size_t n) {
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) snprintf(s.data() + i * 2, 3, "%02x", dig[i]);
    return s;
}

bool HashBuffers(const std::pair<const void*, size_t>* parts, size_t count, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE h{};
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        ok = true;
        for (size_t i = 0; i < count && ok; ++i) {
            if (parts[i].second == 0) continue;
            ok = BCryptHashData(h, static_cast<PUCHAR>(const_cast<void*>(parts[i].first)),
                                 static_cast<ULONG>(parts[i].second), 0) == 0;
        }
        if (ok) ok = BCryptFinishHash(h, out, 32, 0) == 0;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
}  // namespace

std::string Sha256Hex(const void* data, size_t len) {
    unsigned char dig[32];
    std::pair<const void*, size_t> part{data, len};
    if (!HashBuffers(&part, 1, dig)) return {};
    return HexOf(dig, 32);
}

std::string Sha256HexFile(const std::wstring& path) {
    // Full sharing so files the game or the log writer still has open can be read.
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE h{};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD rd = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &rd, nullptr) && rd) {
            ok = BCryptHashData(h, buf.data(), rd, 0) == 0;
        }
        unsigned char dig[32];
        if (ok && BCryptFinishHash(h, dig, 32, 0) == 0) hex = HexOf(dig, 32);
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

std::string ShortSaltedHash(std::string_view salt, std::string_view value) {
    unsigned char dig[32];
    std::pair<const void*, size_t> parts[2] = {{salt.data(), salt.size()}, {value.data(), value.size()}};
    if (!HashBuffers(parts, 2, dig)) return "00000000";
    return HexOf(dig, 4);
}

std::string RandomSalt() {
    unsigned char buf[16];
    if (BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // Fall back to something unique per run rather than failing the export.
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        DWORD mix[4] = {GetCurrentProcessId(), GetTickCount(), static_cast<DWORD>(c.QuadPart),
                        static_cast<DWORD>(c.QuadPart >> 32)};
        memcpy(buf, mix, sizeof(mix));
        memcpy(buf + sizeof(mix), mix, sizeof(buf) - sizeof(mix));
    }
    return HexOf(buf, sizeof(buf));
}

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE h{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
    alg_ = alg;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) return;
    h_ = h;
    ok_ = true;
}

Sha256::~Sha256() {
    if (h_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(h_));
    if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

bool Sha256::Update(const void* data, size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    while (ok_ && len) {
        const ULONG n = static_cast<ULONG>(len > 0x40000000u ? 0x40000000u : len);
        ok_ = BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(h_), const_cast<PUCHAR>(p), n, 0) == 0;
        p += n;
        len -= n;
    }
    return ok_;
}

std::string Sha256::FinishHex() {
    unsigned char dig[32];
    if (!ok_ || BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(h_), dig, 32, 0) != 0) {
        ok_ = false;
        return {};
    }
    ok_ = false;
    return HexOf(dig, 32);
}
}  // namespace melange::hashutil
