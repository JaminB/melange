// The torrent engine of a build without libtorrent (HOLO_TORRENT=OFF): every torrent request fails cleanly.
// Placeholder from Melange 0.12.0 C0; C6b replaces it with bt_libtorrent*.cpp (this file stays for OFF builds).
#include "bt/bt.h"

namespace holo::bt {
std::unique_ptr<Engine> CreateEngine(const Settings&, std::string* err) {
    if (err) *err = "this holo-media build has no torrent support";
    return nullptr;
}
std::string EngineVersion() { return {}; }
}  // namespace holo::bt
