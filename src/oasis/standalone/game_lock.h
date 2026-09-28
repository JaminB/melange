#pragma once
#include <string>

namespace melange::oasis::standalone {
// True while a Melange instance for this game folder is running (its Local\Melange-<hash> mutex is held).
// The hash must match oasis.cpp's CreateInstanceMutex exactly: same folder, same FNV-1a, same format string.
bool GameRunning(const std::wstring& gameDir);
}  // namespace melange::oasis::standalone
