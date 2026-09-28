#pragma once
#include <cstdint>
namespace melange::mods {
enum class Kind : uint8_t { ClientOnly, Content };
enum class State : uint8_t {
    Enabled, Disabled, Blocked, PendingConsent, Incompatible,
    RestartRequired   // content-set change: takes effect at next launch (message ids)
};
struct ModInfo {
    const char* id; const char* name; const char* version; const char* authors;  // authors comma-joined
    const wchar_t* dir;
    Kind kind; State state;
    const char* reason;           // "" unless Blocked/Incompatible/PendingConsent/RestartRequired
    bool implicitManifest;        // M1-era folder without spice.json
    bool hasClient, hasSim;
    bool unsafe, unsafeGranted;   // Deep Desert declared / granted
    int order;                    // load order index (0 = first loaded, lowest override priority)
};
int List(ModInfo* out, int max);          // every discovered mod, load order; returns the total
bool Find(const char* id, ModInfo* out);
bool SetEnabled(const char* id, bool on); // persisted; client-only mods apply immediately
bool SetDeepDesert(const char* id, bool granted);
const wchar_t* ModsDir();
using ChangeFn = void (*)(void* user);
int OnChange(ChangeFn fn, void* user);    // any thread; called on the main thread
void RemoveOnChange(int handle);

// Content identity and the lobby handshake.
struct ContentId {
    char hash[65];        // sha256 hex; "" when vanilla
    uint32_t contentMods, modMessages;
    bool vanilla;         // no content or unsafe mods enabled and no mod message names registered
};
ContentId LocalContent();
enum class PeerStatus : uint8_t { Unknown, Vanilla, MelangeVanilla, Match, Mismatch };
struct Peer { uint64_t steamId; char name[64]; PeerStatus status; char hash16[17]; char version[16]; };
int Peers(Peer* out, int max);  // current lobby members other than us; 0 outside a lobby
bool SimAllowedThisMatch();     // decided once per match VM; true offline when content mods exist
}
