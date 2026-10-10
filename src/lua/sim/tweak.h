#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lua/engine50.h"
#include "melange/weapons.h"

// In-memory weapon container overrides from sim mods (wum.sim.weapon). Containers are found by resource name (clone
// names included) and fields by schema name, with offsets from the engine's class field list (weapons::Field).
// Every write is undone at match end if the container is still the one written.
namespace melange::tweak {
using weapons::FieldType;

enum class TweakError { Ok, UnknownWeapon, UnknownField, Unsupported, TypeMismatch, OutOfRange, WriteFailed, Inexact };
const char* ToString(TweakError e);

// A value of one of the supported field types (F32, I32, U32, U16, U8, Bool, String). `str` is borrowed: for Get it
// points into a buffer that stays valid until the next Get. The Lua value is a float32, so a U32 above 2^24 (16777216)
// is refused with TweakError::Inexact: the float cannot name every integer there. A script's 16777217 is already
// rounded to 16777216 before Set sees it and is accepted as that. Get of a U32 above 2^24 returns the nearest float32.
struct Value {
    FieldType type = FieldType::None;
    float num = 0;
    bool b = false;
    const char* str = nullptr;
};
constexpr size_t kMaxString = 255;
bool Supported(FieldType t);
size_t RawSize(FieldType t);  // bytes a numeric field occupies; 0 for String and unsupported types

// Snapshot/restore bookkeeping. No lua50 calls; memory is touched only through mem:: and the XString helpers.
class Engine {
public:
    struct Touched {
        std::string weapon;
        uintptr_t container;
        uint32_t offset;
        FieldType type;
    };
    bool Read(uintptr_t container, uint32_t offset, FieldType t, Value* out) const;
    // Writes `v`, recording the original the first time (container, offset) is touched since the last RestoreAll().
    bool Write(uintptr_t container, const std::string& weapon, uint32_t offset, FieldType t, const Value& v);
    // At match end: restores each field whose container `resolve` still maps the weapon to; drops the rest.
    using ResolveFn = uintptr_t (*)(const char* weapon, void* user);
    void RestoreAll(ResolveFn resolve, void* user);
    size_t PendingCount() const { return snapshots_.size(); }
    const std::vector<Touched>& TouchedFields() const { return touched_; }
    void Reset() {
        snapshots_.clear();
        touched_.clear();
    }

private:
    struct Snapshot {
        std::string weapon;
        uintptr_t container;
        uint32_t offset;
        FieldType type;
        uint32_t bits;
        std::string text;
    };
    std::vector<Snapshot> snapshots_;
    std::vector<Touched> touched_;
};
Engine& Instance();

// wum.sim.weapon(name).get(field) / .set(field, value).
TweakError Get(const char* weapon, const char* field, Value* out);
TweakError Set(const char* weapon, const char* field, const Value& v);
// Numeric shorthands for f32, i32, u32, u16 and u8 fields (bool reads as 0/1), and the container lookup
// (weapons::Container).
TweakError Get(const char* weapon, const char* field, float* value);
TweakError Set(const char* weapon, const char* field, float value);
uintptr_t FindContainer(const char* weaponName);

void Init();  // the match-end observer (idempotent)
int OpenLibrary(lua50::State* L);
}  // namespace melange::tweak
