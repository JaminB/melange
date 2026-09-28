#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "melange/gamestate.h"

// The readers without the game: every address and engine call comes in through Layout and Engine, so the offline
// self-test runs them against memory it builds itself.
namespace melange::gamestate::detail {
struct Layout {
    uintptr_t wormHandles, teamHandles;  // 16 and 4 resource handles: container = *(*(*(h+4i)+4)+0x1c)
    uintptr_t wormVt, teamVt;            // WormDataContainer, TeamDataContainer
    uintptr_t taskManager;               // table = *(*(*taskManager+0x1c))
};

using EnumCb = bool(__cdecl*)(uintptr_t desc, void* ctx);
struct Engine {                          // null members are skipped
    bool (*getInt)(const char* name, int32_t* out);    // the Rm.cpp GetInt wrapper; match only
    uintptr_t (*getResource)(const char* name);        // an AddRef'd descriptor, or 0
    void (*release)(uintptr_t desc);
    bool (*enumerate)(EnumCb cb, void* ctx);           // false when the store is unavailable
};

struct MatchInfo { bool inMatch, online, suddenDeath; uint32_t serial, turnsStarted; };

bool Copy(uintptr_t addr, void* out, size_t n);        // fault-guarded
template <class T> T Rd(uintptr_t a) {
    T v{};
    Copy(a, &v, sizeof(T));
    return v;
}
bool PeekGuarded(uintptr_t addr, void* out, uint32_t n);  // committed, readable, non-guard pages only

// Game text to UTF-8: valid UTF-8 is kept, anything else is read as Windows-1252. Truncated on a character
// boundary to fit `cap` bytes including the terminator.
void Utf8(std::string_view in, char* out, size_t cap);
void ReadCString(uintptr_t p, char* out, size_t cap);   // a char* in game memory, as Utf8

uintptr_t Container(uintptr_t handles, int i, uintptr_t vt);
void FillSnapshot(const Layout& l, const Engine& e, const MatchInfo& m, Snapshot* out);

VarType TypeOfDescriptor(uintptr_t desc);               // by the descriptor's vtable (never called)
bool ReadVar(uintptr_t desc, Var* out);                 // false: no name
bool NameOf(uintptr_t desc, char* out, size_t cap);
int EnumerateVars(const Engine& e, Var* out, int max, const char* prefix);  // total matching, or -1
bool ReadVar1(const Engine& e, const char* name, Var* out);

struct ClassInfo { EntityKind kind; char type[48]; };
// RTTI is followed only inside this range (the exe image in the game, so a guess from the raw view can never walk
// into a guard page); everything by default.
void SetRttiRange(uintptr_t lo, uintptr_t hi);
bool Rtti(uintptr_t vtable, char* name, size_t cap, bool* payload);  // MSVC RTTI, demangled
ClassInfo Classify(uintptr_t vtable);                   // cached per vtable
std::string Demangle(std::string_view raw);             // ".?AVFoo@@" -> "Foo", ".?AVA@B@@" -> "B::A"
int WalkEntities(const Layout& l, Entity* out, int max);
}  // namespace melange::gamestate::detail
