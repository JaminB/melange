#pragma once
// SHA-256 helpers.
#include <cstdint>
#include <string>
#include <string_view>

namespace melange::hashutil {
// Lowercase hex SHA-256; "" on failure.
std::string Sha256Hex(const void* data, size_t len);
std::string Sha256HexFile(const std::wstring& path);

// 8 hex chars of SHA-256(salt + value): stable within one export, different between exports.
std::string ShortSaltedHash(std::string_view salt, std::string_view value);

std::string RandomSalt();

// Incremental SHA-256 (BCrypt), for data that arrives in pieces.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    bool Update(const void* data, size_t len);
    std::string FinishHex();   // "" on failure; the object cannot be updated afterwards
private:
    void* alg_ = nullptr;
    void* h_ = nullptr;
    bool ok_ = false;
};
}  // namespace melange::hashutil
