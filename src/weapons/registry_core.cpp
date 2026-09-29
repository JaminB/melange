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

void Registry::Configure(const std::vector<manifest::CloneDecl>& decls) {
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

void Registry::RegisterText(Clone& c) {
    std::string name = c.decl.text.name;
    const std::string& help = c.decl.text.help;
    if (name.empty() && !e_.GetText(("Text." + c.decl.base).c_str(), &name)) name.clear();
    c.text = !name.empty() && e_.AddText(c.textKey, name.c_str()) == 0;
    if (c.text) {
        bool own = false;
        for (auto& s : c.decl.set) own |= s.field == "DisplayName";
        uint32_t off = 0;
        if (!own && e_.Field(c.info.container, "DisplayName", &off) == FieldType::String)
            e_.AssignString(c.info.container + off, c.textKey);
    }
    c.help = false;
    if (!help.empty()) {
        c.help = e_.AddText(("HelpText." + c.decl.name + "0").c_str(), help.c_str()) == 0;
    } else {
        for (int i = 0; i < 4; ++i) {
            std::string line;
            if (!e_.GetText(("HelpText." + c.decl.base + std::to_string(i)).c_str(), &line)) break;
            const bool ok = e_.AddText(("HelpText." + c.decl.name + std::to_string(i)).c_str(), line.c_str()) == 0;
            if (i == 0) c.help = ok;
            if (!ok) break;
        }
    }
    if (!c.text || !c.help)
        LOG_WARN("[weapons] %s: panel %s%s%s falls back to %s's", c.info.name, c.text ? "" : "name", !c.text && !c.help ? " and " : "",
                 c.help ? "" : "help", c.decl.base.c_str());
}

bool Registry::Init(std::string* why) {
    std::string local;
    if (!why) why = &local;
    why->clear();
    if (live_ || swapped_ >= 0 || std::any_of(std::begin(cellWritten_), std::end(cellWritten_), [](bool b) { return b; })) {
        LOG_WARN("[weapons] the previous match was not closed: its clone state is dropped");
        Reset();
    }
    if (n_ == 0) {
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
    const bool was = live_;
    Reset();
    e_.EnableHooks(false);
    if (was) LOG_INFO("[weapons] match end: panel cells and name slots restored");
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

uintptr_t Registry::TextName(int32_t id, uintptr_t current, bool help) const {
    if (IsVidValue(id)) {
        const uint32_t k = static_cast<uint32_t>(id) - static_cast<uint32_t>(kVidBase);
        if (live_ && k < static_cast<uint32_t>(n_) && (help ? clones_[k].help : clones_[k].text))
            return reinterpret_cast<uintptr_t>(clones_[k].namePtr);
        return orig_[BaseOf(id)];
    }
    if (swapped_ >= 0 && id == clones_[swapped_].info.base && current == reinterpret_cast<uintptr_t>(clones_[swapped_].namePtr))
        return orig_[id];
    return current;
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
