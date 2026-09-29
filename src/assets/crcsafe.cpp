#include "assets/crcsafe.h"

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "weapons/engine.h"

namespace melange::assets::crcsafe {
namespace {
namespace engine = weapons::engine;

// The table is 89 x {char* path, u32 crc32}; entry 0's crc is known to read 0xed888fb8 on the supported build. A
// mismatch here means the table moved or the build differs, so nothing below is trusted.
constexpr uintptr_t kTable = 0x922508;
constexpr uint32_t kFirstCrc = 0xed888fb8;
constexpr int kExpectedCount = 89;
constexpr int kMaxScan = 256;  // a defensive bound; the walk never runs unbounded even if the table is corrupt

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

bool g_loaded = false, g_ok = false;
std::vector<Entry> g_entries;

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    if (!game::IsKnownBuild()) return;
    if (Rd<uint32_t>(kTable + 4) != kFirstCrc) {
        LOG_ERROR("[assets] the CRC table at %08x does not start with the expected entry: treating every mod path "
                   "as unsafe",
                   static_cast<unsigned>(kTable));
        return;
    }
    std::vector<Entry> out;
    for (int i = 0; i < kMaxScan; ++i) {
        const uintptr_t pathPtr = Rd<uintptr_t>(kTable + 8 * static_cast<uintptr_t>(i));
        if (!pathPtr) break;
        std::string path = engine::ReadCString(pathPtr, 260);
        if (path.empty()) {
            LOG_ERROR("[assets] the CRC table entry %d has an unreadable path: treating every mod path as unsafe", i);
            return;
        }
        out.push_back({std::move(path), Rd<uint32_t>(kTable + 8 * static_cast<uintptr_t>(i) + 4)});
    }
    if (static_cast<int>(out.size()) != kExpectedCount) {
        LOG_ERROR("[assets] the CRC table has %zu entries, expected %d: treating every mod path as unsafe", out.size(),
                   kExpectedCount);
        return;
    }
    g_entries = std::move(out);
    g_ok = true;
}
}  // namespace

bool Available() {
    Load();
    return g_ok;
}

const std::vector<Entry>& Entries() {
    Load();
    return g_entries;
}
}  // namespace melange::assets::crcsafe
