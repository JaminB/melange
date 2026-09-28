#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "melange/gamestate.h"

// JSON forms of the game-state structs (the Oasis `state` and `entities` payloads). Strings are already UTF-8.
namespace melange::gamestate::wire {
const char* KindName(EntityKind k);
bool KindFromName(std::string_view s, EntityKind* out);
const char* TypeName(VarType t);
constexpr uint32_t KindBit(EntityKind k) { return 1u << static_cast<uint32_t>(k); }
constexpr uint32_t kAllKinds = 0x1f;

std::string SnapshotJson(const Snapshot& s, bool available);
std::string EntityJson(const Entity& e);
std::string EntitiesJson(const Entity* e, int n, uint32_t kinds = kAllKinds);
std::string VarJson(const Var& v);
std::string VecJson(const Vec3& v);
}  // namespace melange::gamestate::wire
