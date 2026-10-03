#pragma once

namespace melange::launcher::storehost {
void Install();   // the store.* methods and channel, on the launcher's host
void Sync();      // follow the chosen game folder (main thread)
}  // namespace melange::launcher::storehost
