#pragma once
// Private seams of the event bus, shared by bus*.cpp and tests/bus_selftest.cpp. Mods use melange/bus.h.
#include <cstdint>

#include "melange/bus.h"

namespace melange::bus::detail {
// Engine globals holding the message-name table, its slot count and the post-target service.
// The self-test points them at fakes.
struct RegistrySource {
    uintptr_t tableVar = 0x96d094;
    uintptr_t sizeVar = 0x96d08c;
    uintptr_t postServiceVar = 0x96d090;
};
void SetRegistrySource(const RegistrySource& src);  // also drops every cached name (test only)
const RegistrySource& Source();
int32_t PostTarget();            // *(*(postServiceVar) + 0x14), or -1 if unreadable
const char* SystemName(MsgId id);  // fixed name for an id < 0x8000 ("WindowLoseFocus", "sys:0x40", ...)
size_t UsedSlots();              // non-null registry slots right now

const char* ClassNameOf(uintptr_t vtable);  // "?" if unknown
void RegisterBuiltinDecoders();              // idempotent; called by the first Decode/ClassNameOf

// The inline hooks wrap these so the self-test can drive the same path with fake messages.
// `forward` is the engine function being wrapped.
using PostForward = int (*)(void* msg, void* ctx);
using DeliverForward = int (*)(void* table, void* msg, int handle, char bcast, void* ctx);
int RunPost(void* msg, uintptr_t caller, PostForward forward, void* ctx);
int RunDeliver(void* table, void* msg, int handle, char bcast, uintptr_t caller, DeliverForward forward, void* ctx);

// Per-frame housekeeping: resolves pending SubscribeName()s and frees retired subscriber tables.
void Tick();
size_t PendingNames();
// Size of the engine object from the arena header at raw-4, or 0 if the header looks bogus.
uint32_t ObjectSize(const uint8_t* raw);
}  // namespace melange::bus::detail
