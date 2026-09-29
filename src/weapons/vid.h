#pragma once

// Virtual ids: the selection translation (0x603cdb, and its log read at 0x603cfc) and the id guards of the panel's
// inventory, allowed, delay and can-use reads. Created disabled; enabled only while a match has live clones.
namespace melange::weapons::vid {
bool Create();
bool Enable(bool on);  // true when every hook ends up in the wanted state
}  // namespace melange::weapons::vid
