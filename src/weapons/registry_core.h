#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
    // The NUL-terminated string at addr, cut at max bytes; false if its first byte cannot be read. Byte by byte, so a
    // string that ends just before an unmapped page still comes back, and it runs once per weapon per match.
    virtual bool ReadString(uintptr_t addr, std::string* out, size_t max = 64) {
        out->clear();
        if (!addr) return false;
        while (out->size() < max) {
            char c = 0;
            if (!Read(addr + out->size(), &c, 1)) return !out->empty();
            if (!c) break;
            out->push_back(c);
        }
        return true;
    }
    // The text of an XString field (as AssignString writes it); false if unreadable. The default reads a plain
    // pointer-to-chars, which is only right for a fake; the game engine overrides it.
    virtual bool ReadXString(uintptr_t field, std::string* out) {
        uint32_t p = 0;
        return Read(field, &p, sizeof p) && p && ReadString(p, out, 256);
    }
};

// A vanilla weapon's renamed panel text. The game formats "Text.%s" / "HelpText.%s%d" with the name it reads from its
// name table, so a rename is a pair of string-table entries under a short key of our own ("wt" + the rule index) plus a hook
// that hands the game that key in place of the weapon's name.
struct TextRule {
    manifest::TextDecl decl;
    std::string token;           // the "name" the game formats into the keys; c_str() is stable after Configure
    int id = -1;                 // the weapon's slot in the name table, -1 until resolved for a match
    uintptr_t slot = 0;          // what that slot held when resolved: the hook compares against it
    bool text = false, help = false;  // registered for this match
    // The in-world weapon-name tag reads the container's DisplayName, not the panel keys: while the rename is live that
    // field holds "Text.<token>", and tagOrig is what to put back. tagContainer/tagField are 0 when nothing was written.
    uintptr_t tagContainer = 0, tagField = 0;
    std::string tagOrig;
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
    explicit Registry(Engine& e) : e_(e) { std::fill(std::begin(ruleOf_), std::end(ruleOf_), int16_t{-1}); }
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // The frozen tables; reads the original name slots. texts = the vanilla renames (see TextRule), none by default.
    void Configure(const std::vector<manifest::CloneDecl>& decls, const std::vector<manifest::TextDecl>& texts = {});
    bool Init(std::string* why);                                    // match Init: every clone, or none; then the renames
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
    // Vanilla renames. TextLive: at least one rule is registered for this match, so TextName may answer for vanilla ids.
    bool TextLive() const { return textLive_; }
    int TextCount() const { return static_cast<int>(rules_.size()); }
    int TagCount() const;  // renames whose container DisplayName is currently pointed at the rename
    const TextRule* TextAt(int i) const { return i >= 0 && i < TextCount() ? &rules_[i] : nullptr; }
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
    void ResolveText();
    void ResetText();
    bool SetTag(TextRule& r, uintptr_t container);
    void RestoreTag(TextRule& r);
    uintptr_t Renamed(int32_t id, uintptr_t current, bool help) const;
    bool PutText(const std::string& key, const std::string& value);
    uint32_t BaseIcon(int32_t base);
    void Swap(int k);
    void Unswap();
    void RestoreCells();
    void Reset();
    uint32_t U32(uintptr_t a);
    bool PutU32(uintptr_t a, uint32_t v);

    Engine& e_;
    std::array<Clone, kMaxClones> clones_{};
    int n_ = 0;
    uintptr_t orig_[kEnumCount] = {};
    bool live_ = false;
    int active_ = -1, swapped_ = -1;
    bool cellWritten_[kMaxClones] = {};
    std::vector<TextRule> rules_;
    int16_t ruleOf_[kEnumCount];  // name-table id -> index into rules_, -1 for none
    bool textLive_ = false;
};
}  // namespace melange::weapons::core
