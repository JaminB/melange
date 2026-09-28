#pragma once
// Self-tests of the overlay's pure logic (hotkey parsing, keyboard filter, menu paths); verb `overlay.selftest`.
#include <string>

namespace melange::render {
// Returns the number of failed checks; `report` receives one line per failure plus a summary line.
int RunLogicSelfTests(std::string* report);
}  // namespace melange::render
