#pragma once
// SHA-256 helpers for the log exporter (manifest checksums, plugin identification and the salted
// short hashes used to redact Steam ids / IP addresses, see docs/m0-design.md §5.1 Q4). Private to
// src/tools/; core/game.cpp has its own file-hashing helper it keeps private to itself.
#include <cstdint>
#include <string>
#include <string_view>

namespace wf::hashutil {
// Full lowercase hex SHA-256 of a buffer / file. Returns "" on failure (missing file, BCrypt error).
std::string Sha256Hex(const void* data, size_t len);
std::string Sha256HexFile(const std::wstring& path);

// A short (8 hex char), non-reversible, salted digest: stable for the same (salt, value) pair so the
// same Steam id or IP prints as the same token everywhere inside one export, but does not by itself
// identify the value, and differs between two exports (each gets a fresh random salt).
std::string ShortSaltedHash(std::string_view salt, std::string_view value);

// A fresh random salt for one export run, hex-encoded.
std::string RandomSalt();
}  // namespace wf::hashutil
