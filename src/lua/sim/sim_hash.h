#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Mod hash contributors for the tick hash: for every sim mod of a match, "mod.<id>.env" (a digest of the mod's
// globals and wum.sim.storage) and "mod.<id>.hash" (the values it passed to wum.sim.hash this tick).
namespace melange::simhash {
enum class EnvMode : uint8_t { Off, Changed, Always };  // Changed: re-digest only after the mod's code ran
EnvMode ParseMode(const char* s);                        // "off" | "changed" | "always"; anything else: Changed
const char* ModeName(EnvMode m);
void Install(EnvMode mode);                              // registers the contributors at every match start
void Uninstall();

constexpr int kDepth = 3;                                // nested tables followed below the root
constexpr uint32_t kMaxEntries = 50000;                  // per digest; past it the digest is a fixed marker

// Top-level keys of a mod's globals ('g') or storage ('s') that changed, were added or removed, per tick.
struct Value {
    uint8_t type;                                        // Lua type; bytes hold number bits, bool, string head
    uint8_t len;
    uint32_t fullLen;
    uint8_t bytes[16];
};
struct EnvChange {
    uint32_t match, tick;                                // sim match serial, Wormsign tick
    char mod[48];
    char root;
    uint8_t kind;                                        // 0 changed, 1 added, 2 removed
    Value key, before, after;
    char keyText[32];
    uint64_t beforeHash, afterHash;
};
size_t EnvChanges(uint32_t fromTick, uint32_t toTick, EnvChange* out, size_t max);  // this match; any thread
std::string Format(const EnvChange& c);                  // "mod.x.env global counter: 5 -> 6"

struct Cost {
    uint64_t digests, entries;
    uint32_t p50Us10, p95Us10, maxUs10;                  // 0.1 us units
};
Cost GetCost();
void ResetCost();
}  // namespace melange::simhash
