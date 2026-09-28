#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "melange/jlog.h"

// The in-memory tail behind jlog::Tail(): a power-of-two ring indexed by seq, so a caller asking for lines
// after some seq it already has gets exactly the new ones (O(new lines)), not a scan of the whole tail.
// Pure and self-contained (no game, no file I/O); tests/jlog_selftest.cpp drives it directly.
namespace melange::jlog::ring {

// Rounds `capacity` up to a power of two and (re)allocates empty slots, discarding any content. Called once
// from jlog::internal::Init()/ShutdownForTests(); not safe to call concurrently with Push/Since.
void SetCapacity(size_t capacity);

// Stores `l` at slot (l.seq & mask), replacing whatever line previously lived there. Thread-safe. Callers
// must push in increasing seq order (jlog.cpp assigns seq under its own queue lock before calling this).
void Push(const Line& l);

// The most recently pushed seq, or 0 if nothing has been pushed since the last SetCapacity/Clear. Thread-safe,
// lock-free.
uint64_t Head();

// Appends up to `max` lines with seq > afterSeq, oldest first, and returns how many were appended. A seq that
// fell off the ring's retention is skipped rather than reported as a gap: the caller sees fewer lines than the
// true count, never wrong ones. Thread-safe; the lock is held only for the duration of the copy.
size_t Since(uint64_t afterSeq, std::vector<Line>& out, size_t max);

// Empties the ring without changing its capacity. Thread-safe.
void Clear();

}  // namespace melange::jlog::ring
