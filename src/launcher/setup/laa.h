#pragma once
#include <string>

#include "launcher/setup/engine.h"

// Opt-in large-address-aware (4 GB) mode: sets or clears one bit of WormsMayhem.exe's PE header. Only the supported
// build is touched, the hash that identifies it ignores the bit (core/pe_laa.h), and the change is made on a copy
// that is checked and then swapped in. Melange\laa.json records that Melange set the bit, so only Melange's own
// change is ever reverted on its own. UI-free; the self-test drives it on a fake folder.
namespace melange::launcher::setup {
struct LaaResult {
    bool ok = false;           // the exe is now as asked (or was already, or was left alone on purpose)
    bool changed = false;      // the exe was rewritten
    bool needsAdmin = false;   // Windows refused the write for lack of rights
    std::string state;         // applied | reverted | unchanged | external | refused | failed
    std::string message;       // user copy when not ok
    unsigned long win32 = 0;
};
// Makes the exe large-address-aware (want) or stock. Clearing the bit needs the marker unless `force`: an exe
// patched by something else is left as it is. Never throws; the caller decides whether a failure matters.
LaaResult EnsureLaa(const Context& ctx, bool want, bool force = false);

bool LaaMarkerPresent(const std::wstring& gameDir);
// [Game] LargeAddressAware in Melange.ini (default false).
bool IniWantsLaa(const std::wstring& gameDir);
// Writes [Game] LargeAddressAware=0|1, every other byte of Melange.ini kept. "" or the user copy of the failure.
std::string SetIniLaa(const std::wstring& gameDir, bool on);
}  // namespace melange::launcher::setup
