#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "melange/weapons.h"
#include "weapons/manifest.h"

// The clone registry without the game: creation per match (all clones or none), the panel cells, the name-slot swap,
// the virtual-id mapping the hooks apply, and the clone text. Everything that touches the game goes through Engine,
// so the offline self-test drives it with a fake.
namespace melange::weapons::core {
constexpr uintptr_t kNames = 0x90c920, kPanel = 0x920e58;
constexpr int kEnumCount = 0x45, kPanelCells = 42;
constexpr uint32_t kUndefined = 0x43;
constexpr int32_t kFallbackBase = 1;  // kWeaponBazooka

class Engine {
public:
    virtual ~Engine() = default;
    virtual uintptr_t Lookup(const char* name) = 0;
    virtual uintptr_t LookupDesc(const char* name) = 0;
    virtual int AddResource(const char* name, uintptr_t obj, uint32_t flags) = 0;
    virtual int LoadModBank(const char* mod, const char* rel, std::string* err) = 0;
    virtual uintptr_t ClassOf(uintptr_t container) = 0;
    virtual FieldType Field(uintptr_t container, const char* field, uint32_t* offset) = 0;
    virtual bool Read(uintptr_t addr, void* out, size_t n) = 0;
    virtual bool Write(uintptr_t addr, const void* data, size_t n) = 0;
    virtual bool AssignString(uintptr_t field, const char* s) = 0;
    virtual int AddText(const char* key, const char* value) = 0;
    virtual bool GetText(const char* key, std::string* out) = 0;
    virtual uint32_t ReserveIcon(const char* mod, const char* relPng, std::string* err) = 0;  // 0 = none
    virtual bool HudUsable(const char* mod) = 0;  // the mod's loose-file root is an engine search path
    virtual bool EnableHooks(bool on) = 0;
    virtual uint32_t Tick() = 0;
};

struct Clone {
    CloneInfo info{};
    manifest::CloneDecl decl;
    const char* namePtr = nullptr;  // info.name; its address stands in for a name slot
    char textKey[64] = {};
    bool text = false, help = false, hud = false;
    bool created = false;  // registered in this launch, so a container by that name may be ours
    bool iconAsked = false;
};

class Registry {
public:
    explicit Registry(Engine& e) : e_(e) {}
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    void Configure(const std::vector<manifest::CloneDecl>& decls);  // the frozen table; reads the original name slots
    bool Init(std::string* why);                                    // match Init: every clone, or none
    void MatchEnd();
    void TurnEnded();
    int32_t Select(int32_t value);  // selection handler: the id to store in the worm's selected weapon

    // Hook helpers (no logging, no allocation).
    int32_t GuardId(int32_t id) const;
    uintptr_t TextName(int32_t id, uintptr_t current, bool help) const;
    uintptr_t CanUseSlot(int32_t id, uintptr_t current) const;
    const char* HudName() const;

    int Count() const { return n_; }
    bool Live() const { return live_; }
    int Active() const { return active_; }
    int SwappedBase() const { return swapped_ < 0 ? -1 : clones_[swapped_].info.base; }
    const Clone* At(int k) const { return k >= 0 && k < n_ ? &clones_[k] : nullptr; }
    const CloneInfo* ByDesc(uintptr_t desc) const;
    int32_t BaseOf(int32_t v) const;  // a declared vid's base, else kFallbackBase
    bool IsVidValue(int32_t v) const { return static_cast<uint32_t>(v) >= static_cast<uint32_t>(kVidBase); }

private:
    bool Create(Clone& c, std::string* why);
    bool ApplySet(Clone& c, std::string* why);
    void RegisterText(Clone& c);
    uint32_t BaseIcon(int32_t base);
    void Swap(int k);
    void Unswap();
    void RestoreCells();
    void Drop();
    uint32_t U32(uintptr_t a);
    bool PutU32(uintptr_t a, uint32_t v);

    Engine& e_;
    std::array<Clone, kMaxClones> clones_{};
    int n_ = 0;
    uintptr_t orig_[kEnumCount] = {};
    bool live_ = false;
    int active_ = -1, swapped_ = -1;
    bool cellWritten_[kMaxClones] = {};
};
}  // namespace melange::weapons::core
