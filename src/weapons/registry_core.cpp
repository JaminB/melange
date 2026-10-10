#include "weapons/registry_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace melange::weapons::core {
namespace {
uint32_t Ptr32(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
uintptr_t CellAddr(int cell) { return kPanel + 8 * static_cast<uintptr_t>(cell); }
uintptr_t SlotAddr(int id) { return kNames + 4 * static_cast<uintptr_t>(id); }
}  // namespace

uint32_t Registry::U32(uintptr_t a) {
    uint32_t v = 0;
    e_.Read(a, &v, sizeof v);
    return v;
}

bool Registry::PutU32(uintptr_t a, uint32_t v) { return e_.Write(a, &v, sizeof v); }

void Registry::Configure(const std::vector<manifest::CloneDecl>& decls, const std::vector<manifest::TextDecl>& texts) {
    rules_.clear();
    rules_.reserve(texts.size());  // the hook hands out token.c_str(): the rules must not move after this
    for (const auto& t : texts) {
        TextRule r;
        r.decl = t;
        // Short and fixed-width on purpose: the game formats the token into "HelpText.%s%d" in a buffer of unknown size
        // (a clone name is capped at 48 for the same reason), so nothing from the mod id or the weapon goes into it.
        // The width keeps "HelpText.<token>0" from reading as another token's key.
        char tok[16];
        snprintf(tok, sizeof tok, "wt%03zu", rules_.size());
        r.token = tok;
        rules_.push_back(std::move(r));
    }
    ResetText();
    n_ = static_cast<int>(std::min<size_t>(decls.size(), kMaxClones));
    for (int k = 0; k < n_; ++k) {
        Clone& c = clones_[k];
        const auto& d = decls[k];
        c = Clone{};
        c.decl = d;
        c.info.k = static_cast<uint16_t>(k);
        c.info.vid = kVidBase + k;
        c.info.base = d.baseId;
        strncpy_s(c.info.name, d.name.c_str(), _TRUNCATE);
        strncpy_s(c.info.mod, d.mod.c_str(), _TRUNCATE);
        c.info.cell = static_cast<int8_t>(d.cell);
        c.namePtr = c.info.name;
        snprintf(c.textKey, sizeof c.textKey, "Text.%s", c.info.name);
    }
    for (int id = 0; id < kEnumCount; ++id) orig_[id] = 0;
    orig_[kFallbackBase] = U32(SlotAddr(kFallbackBase));
    for (int k = 0; k < n_; ++k) {
        const int b = clones_[k].info.base;
        if (b > 0 && b < kEnumCount) orig_[b] = U32(SlotAddr(b));
    }
}

uint32_t Registry::BaseIcon(int32_t base) {
    for (int cell = 0; cell < kPanelCells; ++cell)
        if (U32(CellAddr(cell)) == static_cast<uint32_t>(base)) return U32(CellAddr(cell) + 4);
    return 0;
}

bool Registry::ApplySet(Clone& c, std::string* why) {
    const uintptr_t obj = c.info.container;
    for (const auto& s : c.decl.set) {
        uint32_t off = 0;
        const FieldType t = e_.Field(obj, s.field.c_str(), &off);
        if (t == FieldType::None || t != s.type) {
            *why = c.decl.name + ": field " + s.field + " is not settable on this container";
            return false;
        }
        bool ok = false;
        switch (t) {
            case FieldType::F32: {
                const float v = static_cast<float>(s.number);
                ok = e_.Write(obj + off, &v, sizeof v);
                break;
            }
            case FieldType::I32: {
                const int32_t v = static_cast<int32_t>(s.number);
                ok = e_.Write(obj + off, &v, sizeof v);
                break;
            }
            case FieldType::U32: {
                const uint32_t v = static_cast<uint32_t>(s.number);
                ok = e_.Write(obj + off, &v, sizeof v);
                break;
            }
            case FieldType::U16: {
                const uint16_t v = static_cast<uint16_t>(s.number);
                ok = e_.Write(obj + off, &v, sizeof v);
                break;
            }
            case FieldType::U8: {
                const uint8_t v = static_cast<uint8_t>(s.number);
                ok = e_.Write(obj + off, &v, sizeof v);
                break;
            }
            case FieldType::Bool: {
                const uint8_t v = s.boolean ? 1 : 0;
                ok = e_.Write(obj + off, &v, sizeof v);
                break;
            }
            case FieldType::String: ok = e_.AssignString(obj + off, s.string.c_str()); break;
            default: break;
        }
        if (!ok) {
            *why = c.decl.name + ": writing " + s.field + " failed";
            return false;
        }
    }
    return true;
}

bool Registry::Create(Clone& c, std::string* why) {
    const auto& d = c.decl;
    const uintptr_t base = e_.Lookup(d.base.c_str());
    if (!base) {
        *why = d.name + ": base container " + d.base + " not found";
        return false;
    }
    const uintptr_t existing = e_.Lookup(d.name.c_str());
    if (existing && !c.created) {
        *why = d.name + " is already a resource of the game";
        return false;
    }
    if (d.bank.empty()) {
        const int hr = e_.AddResource(d.name.c_str(), base, existing ? 1u : 0u);
        if (hr != 0) {
            char b[32];
            snprintf(b, sizeof b, "%08x", static_cast<unsigned>(hr));
            *why = d.name + ": AddResource returned " + b;
            return false;
        }
    } else if (!existing) {
        std::string err;
        const int hr = e_.LoadModBank(d.mod.c_str(), d.bank.c_str(), &err);
        if (hr != 0) {
            *why = d.name + ": bank " + d.bank + " not loaded" + (err.empty() ? "" : " (" + err + ")");
            return false;
        }
    }
    c.created = true;
    const uintptr_t obj = e_.Lookup(d.name.c_str());
    if (!obj || obj == base) {
        *why = d.name + (d.bank.empty() ? ": the new container was not found" : ": the bank holds no container by that name");
        return false;
    }
    const uintptr_t cls = e_.ClassOf(obj);
    if (!cls || cls != e_.ClassOf(base)) {
        *why = d.name + ": its container class differs from " + d.base + "'s";
        return false;
    }
    const uintptr_t desc = e_.LookupDesc(d.name.c_str());
    if (!desc) {
        *why = d.name + ": no descriptor";
        return false;
    }
    c.info.container = obj;
    c.info.descriptor = desc;
    return ApplySet(c, why);
}

// Overwriting a key left by an earlier match returns non-zero, so a failure is judged by reading the text back.
bool Registry::PutText(const std::string& key, const std::string& value) {
    if (e_.AddText(key.c_str(), value.c_str()) == 0) return true;
    std::string got;
    return e_.GetText(key.c_str(), &got) && got == value;
}

void Registry::RegisterText(Clone& c) {
    std::string name = c.decl.text.name;
    const std::string& help = c.decl.text.help;
    if (name.empty() && !e_.GetText(("Text." + c.decl.base).c_str(), &name)) name.clear();
    c.text = !name.empty() && PutText(c.textKey, name);
    if (c.text) {
        bool own = false;
        for (auto& s : c.decl.set) own |= s.field == "DisplayName";
        uint32_t off = 0;
        if (!own && e_.Field(c.info.container, "DisplayName", &off) == FieldType::String)
            e_.AssignString(c.info.container + off, c.textKey);
    }
    c.help = false;
    if (!help.empty()) {
        c.help = PutText("HelpText." + c.decl.name + "0", help);
    } else {
        for (int i = 0; i < 4; ++i) {
            std::string line;
            if (!e_.GetText(("HelpText." + c.decl.base + std::to_string(i)).c_str(), &line)) break;
            const bool ok = PutText("HelpText." + c.decl.name + std::to_string(i), line);
            if (i == 0) c.help = ok;
            if (!ok) break;
        }
    }
    if (!c.text || !c.help)
        LOG_WARN("[weapons] %s: panel %s%s%s falls back to %s's", c.info.name, c.text ? "" : "name", !c.text && !c.help ? " and " : "",
                 c.help ? "" : "help", c.decl.base.c_str());
}

// The std::string tokens keep their addresses while rules_ is not resized, which only Configure does.
void Registry::ResetText() {
    textLive_ = false;
    std::fill(std::begin(ruleOf_), std::end(ruleOf_), int16_t{-1});
    for (auto& r : rules_) {
        RestoreTag(r);
        r.id = -1;
        r.slot = 0;
        r.text = r.help = false;
    }
}

// The in-world weapon-name tag resolves the container's DisplayName through the string table, so a renamed weapon's
// DisplayName is pointed at the same "Text.<token>" key the panel path registered. Determinism: every peer makes this
// write at the same moment (match creation, behind the same gate as clones) with a cosmetic string key that no
// simulation number reads, and contrib.cpp hashes only clone containers' fields, so the vanilla container is not in the
// Wormsign contribution. A failure costs only the tag; the panel rename stands.
bool Registry::SetTag(TextRule& r, uintptr_t container) {
    uint32_t off = 0;
    if (e_.Field(container, "DisplayName", &off) != FieldType::String) {
        LOG_WARN("[weapons] weaponText %s (%s): no DisplayName string field, the HUD tag keeps its text", r.decl.weapon.c_str(),
                 r.decl.mod.c_str());
        return false;
    }
    const uintptr_t at = container + off;
    std::string orig;
    if (!e_.ReadXString(at, &orig)) {
        LOG_WARN("[weapons] weaponText %s (%s): DisplayName is unreadable, the HUD tag keeps its text", r.decl.weapon.c_str(),
                 r.decl.mod.c_str());
        return false;
    }
    if (!e_.AssignString(at, ("Text." + r.token).c_str())) {
        LOG_WARN("[weapons] weaponText %s (%s): writing DisplayName failed, the HUD tag keeps its text", r.decl.weapon.c_str(),
                 r.decl.mod.c_str());
        return false;
    }
    r.tagContainer = container;
    r.tagField = at;
    r.tagOrig = std::move(orig);
    return true;
}

// Only into the container that was written: a scene change that freed or rebuilt it leaves nothing to restore into.
void Registry::RestoreTag(TextRule& r) {
    if (!r.tagField) return;
    if (e_.Lookup(r.decl.weapon.c_str()) == r.tagContainer) {
        if (!e_.AssignString(r.tagField, r.tagOrig.c_str()))
            LOG_WARN("[weapons] weaponText %s: restoring DisplayName failed", r.decl.weapon.c_str());
    } else {
        LOG_WARN("[weapons] weaponText %s: container changed or gone, DisplayName not restored", r.decl.weapon.c_str());
    }
    r.tagContainer = r.tagField = 0;
    r.tagOrig.clear();
}

int Registry::TagCount() const {
    int n = 0;
    for (const auto& r : rules_) n += r.tagField != 0;
    return n;
}

// Per match, once: find each renamed weapon's slot in the name table (by the name string it holds, which is what the
// panel hook sees), register the strings under the rule's own key, and remember the slot's pointer so the hook is
// one array lookup and one compare.
void Registry::ResolveText() {
    ResetText();
    for (size_t i = 0; i < rules_.size(); ++i) {
        TextRule& r = rules_[i];
        const std::string& w = r.decl.weapon;
        if (!e_.Lookup(w.c_str())) {
            LOG_WARN("[weapons] weaponText %s (%s): no such container in this game, skipped", w.c_str(), r.decl.mod.c_str());
            continue;
        }
        int id = -1;
        uintptr_t slot = 0;
        for (int k = 1; k < kEnumCount && id < 0; ++k) {
            const uintptr_t p = U32(SlotAddr(k));
            std::string have;
            if (p && e_.ReadString(p, &have, 64) && have == w) id = k, slot = p;
        }
        if (id < 0) {
            LOG_WARN("[weapons] weaponText %s (%s): not in the name table, so no panel to rename; skipped", w.c_str(),
                     r.decl.mod.c_str());
            continue;
        }
        if (ruleOf_[id] >= 0) continue;  // the manifest layer refuses two mods renaming one weapon; this is only a guard
        r.text = !r.decl.name.empty() && PutText("Text." + r.token, r.decl.name);
        r.help = !r.decl.help.empty() && PutText("HelpText." + r.token + "0", r.decl.help);
        if (!r.text && !r.help) {
            LOG_WARN("[weapons] weaponText %s (%s): the strings were not registered; the panel keeps its text", w.c_str(),
                     r.decl.mod.c_str());
            continue;
        }
        r.id = id;
        r.slot = slot;
        ruleOf_[id] = static_cast<int16_t>(i);
        textLive_ = true;
        const bool tag = r.text && SetTag(r, e_.Lookup(w.c_str()));  // help-only entries leave DisplayName alone
        LOG_INFO("[weapons] weaponText %s id %d by %s: key %s name=%d help=%d tag=%d", w.c_str(), id, r.decl.mod.c_str(),
                 r.token.c_str(), r.text, r.help, tag);
    }
    if (!textLive_) return;
    int named = 0;
    for (const auto& r : rules_) named += r.text;
    LOG_INFO("[weapons] weaponText: DisplayName set for %d of %d name rename(s)", TagCount(), named);
}

bool Registry::Init(std::string* why) {
    std::string local;
    if (!why) why = &local;
    why->clear();
    if (live_ || swapped_ >= 0 || std::any_of(std::begin(cellWritten_), std::end(cellWritten_), [](bool b) { return b; })) {
        LOG_WARN("[weapons] the previous match was not closed: its clone state is dropped");
        Reset();
    }
    ResetText();
    if (n_ == 0 && rules_.empty()) {
        *why = "no clones declared";
        return false;
    }
    // Init runs only while the gate is open for this match, so every peer with clones declared reaches this point.
    // The id guards stay enabled for as long as that holds, mapping any vid to its base, even if this peer's own
    // clones fail to go live below: a peer whose clones did go live can still select one and send its vid across.
    if (!e_.EnableHooks(true)) {
        *why = "the selection hooks could not be enabled";
        return false;
    }
    // Renames are display only and independent of the clones: they go live even when a clone then fails to.
    ResolveText();
    if (n_ == 0) {
        if (textLive_) return true;
        e_.EnableHooks(false);
        *why = "no weaponText entry could be applied";
        return false;
    }
    for (int k = 0; k < n_; ++k) {
        const int b = clones_[k].info.base;
        if (b <= 0 || b >= kEnumCount || !orig_[b] || U32(SlotAddr(b)) != orig_[b]) {
            *why = std::string(clones_[k].info.name) + ": the name slot of its base is not the original";
            Reset();
            return false;
        }
    }
    for (int k = 0; k < n_; ++k) {
        Clone& c = clones_[k];
        c.info.container = c.info.descriptor = 0;
        c.info.live = false;
        if (!Create(c, why)) {
            Reset();
            return false;
        }
    }
    for (int k = 0; k < n_; ++k) {
        Clone& c = clones_[k];
        RegisterText(c);
        if (!c.iconAsked && !c.decl.panelIcon.empty()) {
            c.iconAsked = true;
            std::string err;
            c.info.iconCode = e_.ReserveIcon(c.info.mod, c.decl.panelIcon.c_str(), &err);
            if (!c.info.iconCode)
                LOG_WARN("[weapons] %s: panel icon %s not used (%s); the base's icon is shown", c.info.name,
                         c.decl.panelIcon.c_str(), err.empty() ? "no free sub-icon" : err.c_str());
        }
        c.hud = !c.decl.hudIcon.empty() && e_.HudUsable(c.info.mod);
    }
    for (int k = 0; k < n_; ++k) {
        const int cell = clones_[k].info.cell;
        if (cell < 0 || cell >= kPanelCells || U32(CellAddr(cell)) != kUndefined || U32(CellAddr(cell) + 4) != 0) {
            *why = std::string(clones_[k].info.name) + ": panel cell " + std::to_string(cell) + " is not free";
            Reset();
            return false;
        }
    }
    for (int k = 0; k < n_; ++k) {
        Clone& c = clones_[k];
        const uint32_t icon = c.info.iconCode ? c.info.iconCode : BaseIcon(c.info.base);
        const uintptr_t a = CellAddr(c.info.cell);
        cellWritten_[k] = true;
        if (!PutU32(a + 4, icon) || !PutU32(a, static_cast<uint32_t>(c.info.vid))) {
            *why = std::string(c.info.name) + ": writing panel cell " + std::to_string(c.info.cell) + " failed";
            Reset();
            return false;
        }
    }
    live_ = true;
    active_ = -1;
    for (int k = 0; k < n_; ++k) {
        Clone& c = clones_[k];
        c.info.live = true;
        LOG_INFO("[weapons] clone k=%d %s (%s) vid %x cell %d icon %x container %08x desc %08x text=%d help=%d hud=%d", k,
                 c.info.name, c.decl.base.c_str(), static_cast<unsigned>(c.info.vid), c.info.cell,
                 static_cast<unsigned>(U32(CellAddr(c.info.cell) + 4)), static_cast<unsigned>(c.info.container),
                 static_cast<unsigned>(c.info.descriptor), c.text, c.help, c.hud);
    }
    return true;
}

void Registry::Swap(int k) {
    if (swapped_ == k) return;
    Unswap();
    const Clone& c = clones_[k];
    if (PutU32(SlotAddr(c.info.base), Ptr32(c.namePtr))) swapped_ = k;
}

void Registry::Unswap() {
    if (swapped_ < 0) return;
    const int b = clones_[swapped_].info.base;
    PutU32(SlotAddr(b), static_cast<uint32_t>(orig_[b]));
    swapped_ = -1;
}

void Registry::RestoreCells() {
    for (int k = 0; k < n_; ++k) {
        if (!cellWritten_[k]) continue;
        const uintptr_t a = CellAddr(clones_[k].info.cell);
        PutU32(a, kUndefined);
        PutU32(a + 4, 0);
        cellWritten_[k] = false;
    }
}

void Registry::Reset() {
    Unswap();
    RestoreCells();
    live_ = false;
    active_ = -1;
    for (int k = 0; k < n_; ++k) {
        clones_[k].info.live = false;
        clones_[k].info.container = clones_[k].info.descriptor = 0;
    }
}

void Registry::MatchEnd() {
    const bool was = live_, text = textLive_;
    Reset();
    ResetText();
    e_.EnableHooks(false);
    if (was) LOG_INFO("[weapons] match end: panel cells and name slots restored");
    else if (text) LOG_INFO("[weapons] match end: weaponText off");
}

void Registry::TurnEnded() {
    if (!live_ || (active_ < 0 && swapped_ < 0)) return;
    Unswap();
    active_ = -1;
    LOG_INFO("[weapons] turn ended: name slot restored (tick %u)", e_.Tick());
}

int32_t Registry::Select(int32_t value) {
    if (!IsVidValue(value)) {
        if (swapped_ >= 0 || active_ >= 0) {
            Unswap();
            active_ = -1;
            LOG_INFO("[weapons] select %d: clone off (tick %u)", value, e_.Tick());
        }
        return value;
    }
    const uint32_t k = static_cast<uint32_t>(value) - static_cast<uint32_t>(kVidBase);
    if (live_ && k < static_cast<uint32_t>(n_)) {
        const Clone& c = clones_[k];
        Swap(static_cast<int>(k));
        active_ = static_cast<int>(k);
        LOG_INFO("[weapons] SELECT vid %x -> +0xf4=%d (%s, tick %u)", static_cast<unsigned>(value), c.info.base, c.info.name,
                 e_.Tick());
        return c.info.base;
    }
    Unswap();
    active_ = -1;
    const int32_t base = BaseOf(value);
    LOG_ERROR("[weapons] virtual id %x selected with no live clone: mapped to base %d (tick %u)", static_cast<unsigned>(value),
              base, e_.Tick());
    return base;
}

int32_t Registry::BaseOf(int32_t v) const {
    if (!IsVidValue(v)) return v;
    const uint32_t k = static_cast<uint32_t>(v) - static_cast<uint32_t>(kVidBase);
    return k < static_cast<uint32_t>(n_) ? clones_[k].info.base : kFallbackBase;
}

int32_t Registry::GuardId(int32_t id) const { return IsVidValue(id) ? BaseOf(id) : id; }

// A vanilla weapon with a live rename: the key is handed over only while the name the game read is still the slot's
// original pointer, so a name some other code swapped in is left alone.
uintptr_t Registry::Renamed(int32_t id, uintptr_t current, bool help) const {
    if (!textLive_ || id < 0 || id >= kEnumCount) return current;
    const int r = ruleOf_[id];
    if (r < 0) return current;
    const TextRule& t = rules_[static_cast<size_t>(r)];
    if (current != t.slot || !(help ? t.help : t.text)) return current;
    return reinterpret_cast<uintptr_t>(t.token.c_str());
}

uintptr_t Registry::TextName(int32_t id, uintptr_t current, bool help) const {
    if (IsVidValue(id)) {
        const uint32_t k = static_cast<uint32_t>(id) - static_cast<uint32_t>(kVidBase);
        if (live_ && k < static_cast<uint32_t>(n_) && (help ? clones_[k].help : clones_[k].text))
            return reinterpret_cast<uintptr_t>(clones_[k].namePtr);
        // The vanilla pointer, like RegisterText's copy of the vanilla text: a clone with no text of its own shows its
        // base's original name even when the base is renamed (the rename belongs to the base's own cell).
        return orig_[BaseOf(id)];
    }
    if (swapped_ >= 0 && id == clones_[swapped_].info.base && current == reinterpret_cast<uintptr_t>(clones_[swapped_].namePtr))
        return Renamed(id, orig_[id], help);
    return Renamed(id, current, help);
}

uintptr_t Registry::CanUseSlot(int32_t id, uintptr_t current) const {
    if (IsVidValue(id)) {
        const uint32_t k = static_cast<uint32_t>(id) - static_cast<uint32_t>(kVidBase);
        if (live_ && k < static_cast<uint32_t>(n_)) return reinterpret_cast<uintptr_t>(&clones_[k].namePtr);
        return reinterpret_cast<uintptr_t>(&orig_[BaseOf(id)]);
    }
    if (swapped_ >= 0 && id == clones_[swapped_].info.base) return reinterpret_cast<uintptr_t>(&orig_[id]);
    return current;
}

const char* Registry::HudName() const {
    if (!live_ || active_ < 0 || !clones_[active_].hud) return nullptr;
    return clones_[active_].decl.hudIcon.c_str();
}

const CloneInfo* Registry::ByDesc(uintptr_t desc) const {
    if (!live_ || !desc) return nullptr;
    for (int k = 0; k < n_; ++k)
        if (clones_[k].info.descriptor == desc) return &clones_[k].info;
    return nullptr;
}
}  // namespace melange::weapons::core
