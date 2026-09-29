#pragma once

// The panel's name and help lookups (0x600e21, 0x5fee4d): a clone cell shows the clone's own Text./HelpText.
// strings, and the base's cell keeps the base's while its name slot is swapped.
namespace melange::weapons::panel {
bool Create();
bool Enable(bool on);
}  // namespace melange::weapons::panel
