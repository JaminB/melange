#pragma once
// Text redaction for the log exporter.
#include <string>
#include <string_view>

namespace melange::redact {
// Case-insensitive replace of `userName` with "%USERNAME%"; names shorter than 3 chars are left alone.
std::string RedactUserName(std::string_view text, std::string_view userName);

std::string ReplaceName(std::string_view text, std::string_view needle, std::string_view token);

// Replaces IPv4 addresses and 17-digit SteamID64s with "hash:<8 hex chars>".
std::string HashIdsAndIps(std::string_view text, std::string_view salt);
}  // namespace melange::redact
