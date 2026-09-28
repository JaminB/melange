#pragma once
// SHA-256 helpers for the log exporter.
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
}  // namespace melange::hashutil
