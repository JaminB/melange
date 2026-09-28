#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lua/engine50.h"

// In-memory WEAPTWK field overrides from sim mods. The match lifecycle comes from lua50::OnContext;
// containers are looked up with 0x50b8b0.
namespace melange::tweak {

// A field of a WEAPTWK container class as named in src/xom/xom_schema.inc. `numeric` marks the F32
// fields our get/set support; the rest exist so an unsupported-but-real field name gets a precise
// error instead of being confused with a typo.
struct SchemaField {
    const char* name;
    bool numeric;  // true only for Type::F32 in the schema
};

// One (weapon name, container class) pairing we know how to resolve, and that class's fields.
struct WeaponClass {
    const char* weapon;          // the container-lookup name, e.g. "kWeaponBazooka"
    const char* containerClass;  // xom_schema.inc class name, for error messages
    const SchemaField* fields;
    int fieldCount;
};
const WeaponClass* FindWeaponClass(const char* weapon);  // nullptr if this weapon has no mapping yet

// A field whose byte offset inside the class was confirmed at runtime, e.g.
// WormDamageMagnitude at +0x15c on the Bazooka's PayloadWeaponPropertiesContainer. A field can be
// `numeric` in the schema and still be missing here: it just has not been RE'd yet.
struct VerifiedOffset {
    const char* containerClass;
    const char* field;
    uint32_t offset;
};
const VerifiedOffset* FindVerifiedOffset(const char* containerClass, const char* field);

// Raw container lookup (0x50b8b0), guarded by a prologue check; 0 on an unknown build, a missing
// container, or a prologue mismatch.
uintptr_t FindContainer(const char* weaponName);

enum class TweakError { Ok, UnknownWeapon, UnknownField, NotNumeric, NoVerifiedOffset, NoContainer };
const char* ToString(TweakError e);

// Snapshot/restore bookkeeping. Pure logic (offline-testable with a fake `container` pointer): no
// lua50 or mem:: calls happen above ReadField/WriteField, which is where real process memory is
// touched.
class Engine {
public:
    bool ReadField(uintptr_t container, uint32_t offset, float* out) const;
    // Writes value, recording the pre-tweak bits the first time (weapon, offset) is touched since
    // the last RestoreAll(); later calls in the same match just update the live value.
    bool WriteField(uintptr_t container, const std::string& weapon, uint32_t offset, float value);
    // Called from the match-end OnContext observer: re-resolves each touched weapon's container
    // (via `resolve`, injected so the offline test can fake it) and restores only if it still
    // points at the object the tweak was made on; otherwise the entry is dropped, since the
    // next scene reloads the container fresh.
    using ResolveFn = uintptr_t (*)(const char* weapon, void* user);
    void RestoreAll(ResolveFn resolve, void* user);
    size_t PendingCount() const { return snapshots_.size(); }
    void Reset() { snapshots_.clear(); }  // test-only

private:
    struct Snapshot {
        std::string weapon;
        uint32_t offset;
        uintptr_t container;
        uint32_t originalBits;
    };
    std::vector<Snapshot> snapshots_;
};

// The process-wide engine used by Get/Set/Init below; exposed so the private test module can read
// PendingCount() and so a future sim_api.cpp has one obvious instance to call into.
Engine& Instance();

// High-level entry points behind wum.sim.weapon(name):get(field) / :set(field, value). Resolve the
// weapon and container, validate the field against the schema and the verified-offset table, and
// read/write through Instance(). `value` is unused for Get.
TweakError Get(const char* weapon, const char* field, float* value);
TweakError Set(const char* weapon, const char* field, float value);

// Wires this module's own OnContext observer (idempotent); call once from SimTweak::Install().
void Init();

// Pushes `wum.sim.weapon(name)` onto the 5.0.1 stack: it returns a table with `get(field)` and
// `set(field, value)` bound to that weapon. SimTweak installs it through simbridge::AddSimFunction.
int OpenLibrary(lua50::State* L);

}  // namespace melange::tweak
