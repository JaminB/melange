// Event bus (component B): the engine's message-name registry, read directly from memory.
//
// The registry is a char*[*0x96d08c] at *0x96d094 (capacity 1300, ~1227 used after start-up); a registered name
// in slot s has message id 0x8000 | s. We never call the engine's lookup 0x690d44: it logs "Unable to find
// message" on every miss. Ids depend on registration order, so everything resolves them by name at run time.
// See docs/m0-design.md §1.2 and §3.B.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/bus_internal.h"
#include "core/mem.h"

namespace wf::bus {
namespace {
detail::RegistrySource g_src;

// Per-id name cache. Registry names have static lifetime [I: every observed name pointer is in .rdata or a
// static table], so a non-null slot pointer is cached for good. A null slot is not cached: mods (M5) may
// register it later. System names and the "?xxxx" placeholders are generated once and leaked (static lifetime
// is part of the contract of MessageView::name).
std::atomic<const char*> g_names[0x10000];
std::atomic<const char*> g_unknown[0x8000];
std::atomic<bool> g_ready{false};

constexpr uint32_t kMaxSlots = 0x8000;  // ids are 0x8000 | slot

const char* Publish(std::atomic<const char*>& slot, const char* made) {
    const char* expected = nullptr;
    if (slot.compare_exchange_strong(expected, made)) return made;
    delete[] made;  // another thread won; its string is identical
    return expected;
}

const char* MakeString(const char* fmt, unsigned v) {
    char* s = new char[16];
    snprintf(s, 16, fmt, v);
    return s;
}

bool ReadTable(uintptr_t& table, uint32_t& size) {
    table = 0;
    size = 0;
    return mem::SafeRead(g_src.tableVar, &table, sizeof(table)) && mem::SafeRead(g_src.sizeVar, &size, sizeof(size));
}

// SEH-guarded string comparison against an engine string (a slot pointer we have already validated).
bool GuardedEquals(const char* engine, const char* want, size_t n) {
    __try {
        return strncmp(engine, want, n) == 0 && engine[n] == '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ParseHex(std::string_view s, uint32_t& out) {
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v * 16 + static_cast<uint32_t>(d);
    }
    out = v;
    return true;
}

struct SysName {
    MsgId id;
    const char* name;
};
// [V] 0x103/0x104 from XomWndProc 0x7c43f2, 0x1004 Win32MouseEvent (0x701ae6). 0x40/0x41/0x42 are entity-internal
// (0x68b79e sets or clears entity+0x14) and keep the generic "sys:0x40" form [I].
constexpr SysName kSysNames[] = {
    {0x103, "WindowLoseFocus"},
    {0x104, "WindowGainFocus"},
    {0x1004, "Win32MouseEvent"},
};
}  // namespace

namespace detail {
void SetRegistrySource(const RegistrySource& src) {
    g_src = src;
    g_ready = false;
    for (uint32_t i = 0; i < kMaxSlots; ++i) g_names[0x8000 | i].store(nullptr);  // leaks nothing: engine pointers
}
const RegistrySource& Source() { return g_src; }

int32_t PostTarget() {
    uintptr_t svc = 0;
    int32_t h = -1;
    if (!mem::SafeRead(g_src.postServiceVar, &svc, sizeof(svc)) || !svc) return -1;
    if (!mem::SafeRead(svc + 0x14, &h, sizeof(h))) return -1;
    return h;
}

const char* SystemName(MsgId id) {
    id &= 0x7fff;
    if (const char* s = g_names[id].load(std::memory_order_acquire)) return s;
    for (const auto& e : kSysNames)
        if (e.id == id) {
            g_names[id].store(e.name, std::memory_order_release);
            return e.name;
        }
    return Publish(g_names[id], MakeString("sys:0x%x", id));
}

const char* RegistrySlot(size_t slot) {
    if (slot >= kMaxSlots) return nullptr;
    auto& cache = g_names[0x8000 | slot];
    if (const char* s = cache.load(std::memory_order_acquire)) return s;
    uintptr_t table = 0;
    uint32_t size = 0;
    if (!ReadTable(table, size) || !table || slot >= size) return nullptr;
    uintptr_t p = 0;
    char probe = 0;
    if (!mem::SafeRead(table + slot * sizeof(uint32_t), &p, sizeof(uint32_t)) || !p || !mem::SafeRead(p, &probe, 1))
        return nullptr;
    cache.store(reinterpret_cast<const char*>(p), std::memory_order_release);
    return reinterpret_cast<const char*>(p);
}

size_t UsedSlots() {
    size_t n = Capacity(), used = 0;
    for (size_t i = 0; i < n; ++i)
        if (RegistrySlot(i)) ++used;
    return used;
}
}  // namespace detail

bool RegistryReady() {
    if (g_ready.load(std::memory_order_acquire)) return true;
    uintptr_t table = 0;
    uint32_t size = 0;
    if (!ReadTable(table, size) || !table || !size || size > kMaxSlots) return false;
    for (uint32_t i = 0; i < size; ++i)
        if (detail::RegistrySlot(i)) {
            g_ready.store(true, std::memory_order_release);
            return true;
        }
    return false;
}

size_t Capacity() {
    uintptr_t table = 0;
    uint32_t size = 0;
    if (!ReadTable(table, size) || size > kMaxSlots) return 0;
    return size;
}

const char* NameOf(MsgId id) {
    if (!(id & 0x8000)) return detail::SystemName(id);
    if (const char* s = detail::RegistrySlot(id & 0x7fff)) return s;
    auto& u = g_unknown[id & 0x7fff];
    if (const char* s = u.load(std::memory_order_acquire)) return s;
    return Publish(u, MakeString("?%04x", id));
}

MsgId IdOf(std::string_view name) {
    if (name.empty()) return kInvalidId;
    // System ids are not in the registry but have fixed names; accept them so NameOf/IdOf round-trip for every
    // id a MessageView can carry (C's deny-list names "sys:0x1004").
    if (name.size() > 6 && name.substr(0, 6) == "sys:0x") {
        uint32_t v = 0;
        return ParseHex(name.substr(6), v) && v < 0x8000 ? static_cast<MsgId>(v) : kInvalidId;
    }
    for (const auto& e : kSysNames)
        if (name == e.name) return e.id;
    if (name[0] == '?') return kInvalidId;  // placeholder for an empty slot: not registered by definition
    const size_t n = Capacity();
    for (size_t i = 0; i < n; ++i) {
        const char* s = detail::RegistrySlot(i);
        if (s && s[0] == name[0] && GuardedEquals(s, name.data(), name.size())) return static_cast<MsgId>(0x8000 | i);
    }
    return kInvalidId;
}
}  // namespace wf::bus
