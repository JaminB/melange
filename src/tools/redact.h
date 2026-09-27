#pragma once
// Text redaction for the log exporter, per docs/m0-design.md §5.1 Q4:
//   - the Windows user name is replaced with %USERNAME% (opt.redactUserPaths, default on);
//   - Steam ids and IP addresses are always replaced with a short hash, stable within one export
//     (same salt) and different between exports (a fresh random salt each time);
//   - the computer name is left alone (excluded from redaction on purpose).
// Private to src/tools/.
#include <string>
#include <string_view>

namespace wf::redact {
// Case-insensitive whole-string substring replace of `userName` with "%USERNAME%". A no-op for a
// user name shorter than 3 characters, to cut down on false positives on short/common names.
std::string RedactUserName(std::string_view text, std::string_view userName);

// Replaces IPv4 dotted-quad addresses and 17-digit SteamID64s (76561...) with "hash:<8 hex chars>"
// via wf::hashutil::ShortSaltedHash(salt, match).
std::string HashIdsAndIps(std::string_view text, std::string_view salt);
}  // namespace wf::redact
