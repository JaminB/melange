#pragma once
// Offline-capable self-tests of the overlay's pure logic (hotkey parsing, the DirectInput keyboard filter, menu
// paths). Run in-game by the Automation verb `overlay.selftest` and offline by scripts/test-overlay.ps1.
#include <string>

namespace melange::render {
// Returns the number of failed checks; `report` receives one line per failure plus a summary line.
int RunLogicSelfTests(std::string* report);
}  // namespace melange::render
