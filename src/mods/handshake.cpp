// Handshake: content identity, lobby member data and the host's sim switch (scaffold stub).
#include "core/module.h"
#include "melange/mods.h"

namespace melange::mods {
ContentId LocalContent() {
    ContentId c{};
    c.vanilla = true;
    return c;
}
int Peers(Peer*, int) { return 0; }
bool SimAllowedThisMatch() { return false; }
}  // namespace melange::mods

namespace {
class Handshake final : public melange::Module {
public:
    const char* Name() const override { return "Handshake"; }
    const char* Description() const override { return "content mod identity and the online lobby handshake"; }
    int Order() const override { return 57; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(Handshake);
