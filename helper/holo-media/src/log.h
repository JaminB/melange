#pragma once
// The helper's log: <cache>\holo-media.log (the only folder it may write at Low integrity), at most 2 MB, then quiet.
// Thread-safe; before Open (or without a cache folder) lines go to stderr. Never logs a URL a peer chose beyond the
// canonical form, and never a path outside the pack and the cache.
#include <string>

namespace holo::log {
void Open(const std::wstring& path);
void Line(const char* fmt, ...);
}  // namespace holo::log
#define HLOG(...) ::holo::log::Line(__VA_ARGS__)
