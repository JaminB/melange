#include "wormsign/enginecheck.h"

#include <safetyhook.hpp>

#include <cstring>
#include <mutex>

#include "core/log.h"
#include "core/mem.h"
#include "melange/jlog.h"
#include "melange/wormsign.h"
#include "tools/json_mini.h"

namespace melange::wormsign::enginecheck {
namespace {
constexpr uintptr_t kInitialise = 0x68a027;  // __thiscall GameStateValidationMsg::Initialise(this, sov)
constexpr uintptr_t kValidate = 0x68a335;    // bool __thiscall GameStateValidationMsg::ValidateNow(this, sov)
constexpr uintptr_t kAbortFunnel = 0x7092cb; // __thiscall(NetService, const char* key, detail): logs, AbortGame
// Each "push offset '  Reason n  '" inside ValidateNow, n = index + 1.
constexpr uintptr_t kReasonSite[13] = {0x68a447, 0x68a4b6, 0x68a517, 0x68a5ba, 0x68a612, 0x68a66d, 0x68a6c1,
                                       0x68a711, 0x68a75d, 0x68a7a7, 0x68a813, 0x68a96a, 0x68a9a8};
constexpr uint32_t kReasonStr[13] = {0x8847a0, 0x88474c, 0x8846e8, 0x884644, 0x884600, 0x8845c0, 0x884578,
                                     0x884534, 0x8844e8, 0x8844a0, 0x884468, 0x88442c, 0x8843c0};
constexpr size_t kMaxRecords = 4096;

SafetyHookInline g_hInit, g_hVal;
SafetyHookMid g_mAbort, g_mReason[13];
bool g_installed = false, g_active = false;
std::mutex g_mx;
std::vector<Record> g_recs;
thread_local bool t_inCheck = false;
thread_local uint16_t t_reasons = 0;

void Add(Kind kind, uint8_t sov, bool result, uint16_t reasons, const char* error) {
    Record r{};
    r.kind = kind;
    r.serial = MatchSerial();
    r.tick = Tick();
    r.timeMs = LogicTimeMs();
    r.sov = sov;
    r.result = result;
    r.reasons = reasons;
    if (error) strncpy_s(r.error, error, _TRUNCATE);
    {
        std::lock_guard lk(g_mx);
        if (g_recs.size() >= kMaxRecords) g_recs.erase(g_recs.begin());
        g_recs.push_back(r);
    }
    // "reasons" stays the bitmask; "reason" names every failed check (one text when one failed, as before) and
    // "reasonList" their numbers, so a camera + worm failure is not reported as the camera alone.
    jlog::Rec("wormsign", result ? jlog::Level::Info : jlog::Level::Warn,
              kind == Kind::Build ? "engine validation sent" : kind == Kind::Check ? "engine validation" : "engine abort")
        .Uint("serial", r.serial).Uint("tick", r.tick).Uint("t", r.timeMs).Uint("sov", sov).Bool("ok", result)
        .Uint("reasons", reasons).Str("reasonList", ReasonNumbers(reasons)).Str("reason", ReasonTexts(reasons))
        .Str("error", r.error);
    if (!result) {
        const std::string why = reasons ? DescribeReasons(reasons) : std::string(r.error);
        LOG_WARN("[wormsign] engine %s failed at tick %u (t=%u)%s%s", kind == Kind::Abort ? "match check" : "validation",
                 r.tick, r.timeMs, why.empty() ? "" : ", ", why.c_str());
    }
}

uint32_t __fastcall HkInitialise(void* self, void*, int sov) {
    const uint32_t r = g_hInit.thiscall<uint32_t>(self, sov);
    Add(Kind::Build, static_cast<uint8_t>(sov), true, 0, nullptr);
    return r;
}

uint32_t __fastcall HkValidate(void* self, void*, int sov) {
    t_inCheck = true;
    t_reasons = 0;
    const uint32_t r = g_hVal.thiscall<uint32_t>(self, sov);
    t_inCheck = false;
    Add(Kind::Check, static_cast<uint8_t>(sov), (r & 0xff) != 0, t_reasons, nullptr);
    return r;
}

template <int N>
void OnReason(safetyhook::Context&) {
    if (t_inCheck) t_reasons |= static_cast<uint16_t>(1u << N);
}
using ReasonFn = void (*)(safetyhook::Context&);
constexpr ReasonFn kReasonFn[13] = {&OnReason<0>, &OnReason<1>, &OnReason<2>, &OnReason<3>, &OnReason<4>,
                                     &OnReason<5>, &OnReason<6>, &OnReason<7>, &OnReason<8>, &OnReason<9>,
                                     &OnReason<10>, &OnReason<11>, &OnReason<12>};

void OnAbort(safetyhook::Context& c) {
    uintptr_t key = 0;
    char text[40] = {};
    if (mem::SafeRead(c.esp + 4, &key, sizeof key) && key) {
        for (size_t i = 0; i + 1 < sizeof text; ++i) {
            char ch = 0;
            if (!mem::SafeRead(key + i, &ch, 1) || !ch) break;
            text[i] = ch;
        }
    }
    Add(Kind::Abort, 0, false, 0, text[0] ? text : "unknown");
}

bool BytesMatch() {
    if (!mem::Expect(kInitialise, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x64, 0x53, 0x56, 0x57, 0x8b, 0xf1}) ||
        !mem::Expect(kValidate, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x74, 0x53, 0x56, 0x57, 0x8b, 0xf9}) ||
        !mem::Expect(kAbortFunnel, {0x56, 0x8b, 0xf1, 0xe8, 0x8b, 0x3b, 0x00, 0x00}))
        return false;
    for (int i = 0; i < 13; ++i) {
        const uint32_t s = kReasonStr[i];
        if (!mem::Expect(kReasonSite[i], {0x68, static_cast<int>(s & 0xff), static_cast<int>((s >> 8) & 0xff),
                                          static_cast<int>((s >> 16) & 0xff), static_cast<int>(s >> 24)}))
            return false;
    }
    return true;
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!BytesMatch()) {
        LOG_WARN("[wormsign] engine validation code differs from build #1077: engine checks are not recorded");
        return false;
    }
    using IF = safetyhook::InlineHook::Flags;
    using MF = safetyhook::MidHook::Flags;
    g_hInit = safetyhook::create_inline(kInitialise, &HkInitialise, IF::StartDisabled);
    g_hVal = safetyhook::create_inline(kValidate, &HkValidate, IF::StartDisabled);
    g_mAbort = safetyhook::create_mid(kAbortFunnel, &OnAbort, MF::StartDisabled);
    bool ok = g_hInit && g_hVal && g_mAbort;
    for (int i = 0; ok && i < 13; ++i) {
        g_mReason[i] = safetyhook::create_mid(kReasonSite[i], kReasonFn[i], MF::StartDisabled);
        ok = static_cast<bool>(g_mReason[i]);
    }
    if (!ok) {
        LOG_ERROR("[wormsign] hooking the engine validation failed: engine checks are not recorded");
        Uninstall();
        return false;
    }
    g_installed = true;
    return true;
}

bool SetActive(bool on) {
    if (!g_installed || g_active == on) return g_installed;
    bool ok = true;
    if (on) {
        for (auto& m : g_mReason) ok &= m.enable().has_value();
        ok &= g_mAbort.enable().has_value();
        ok &= g_hVal.enable().has_value();
        ok &= g_hInit.enable().has_value();
    } else {
        ok &= g_hInit.disable().has_value();
        ok &= g_hVal.disable().has_value();
        ok &= g_mAbort.disable().has_value();
        for (auto& m : g_mReason) ok &= m.disable().has_value();
    }
    g_active = on;
    if (!ok) LOG_ERROR("[wormsign] %s the engine validation hooks failed", on ? "enabling" : "disabling");
    return ok;
}

void Uninstall() {
    SetActive(false);
    for (auto& m : g_mReason) m = {};
    g_mAbort = {};
    g_hVal = {};
    g_hInit = {};
    g_installed = false;
}

std::vector<Record> Records(uint32_t serial) {
    std::vector<Record> out;
    std::lock_guard lk(g_mx);
    for (const Record& r : g_recs)
        if (r.serial == serial) out.push_back(r);
    return out;
}

bool FirstFailure(uint32_t serial, Record* out) {
    std::lock_guard lk(g_mx);
    for (const Record& r : g_recs)
        if (r.serial == serial && !r.result) {
            *out = r;
            return true;
        }
    return false;
}

const char* ReasonText(int reason) {
    static const char* const kText[13] = {
        "source of validation differs", "no active worm", "worm indexes are different", "task list mismatch",
        "Random's dont match", "active camera's name differs", "camera's active view matrix differs",
        "camera's logical position differs", "camera's logical target position differs",
        "camera's logical up vector differs", "worm data mismatch", "weapon inventory mismatch",
        "local and remote worm data differ"};
    return reason >= 1 && reason <= 13 ? kText[reason - 1] : "";
}

int FirstReason(uint16_t reasons) {
    for (int i = 0; i < 13; ++i)
        if (reasons & (1u << i)) return i + 1;
    return 0;
}

std::string ReasonNumbers(uint16_t reasons) {
    std::string s;
    for (int i = 0; i < 13; ++i)
        if (reasons & (1u << i)) s += (s.empty() ? "" : ",") + std::to_string(i + 1);
    return s;
}

std::string ReasonTexts(uint16_t reasons) {
    std::string s;
    for (int i = 0; i < 13; ++i)
        if (reasons & (1u << i)) s += (s.empty() ? "" : "; ") + std::string(ReasonText(i + 1));
    return s;
}

std::string DescribeReasons(uint16_t reasons) {
    const std::string n = ReasonNumbers(reasons);
    if (n.empty()) return {};
    return (n.find(',') == std::string::npos ? "reason " : "reasons ") + n + ": " + ReasonTexts(reasons);
}

std::string Json(const std::vector<Record>& v) {
    jsonmini::Arr a;
    for (const Record& r : v) {
        jsonmini::Arr reasons;
        for (int i = 0; i < 13; ++i)
            if (r.reasons & (1u << i)) reasons.Raw(std::to_string(i + 1));
        jsonmini::Obj o;
        o.Str("kind", r.kind == Kind::Build ? "build" : r.kind == Kind::Check ? "check" : "abort")
            .UInt("tick", r.tick).UInt("t", r.timeMs).UInt("sov", r.sov).Bool("ok", r.result)
            .Raw("reasons", reasons.End()).Str("reason", ReasonTexts(r.reasons)).Str("error", r.error);
        a.Raw(o.End());
    }
    return a.End();
}
}  // namespace melange::wormsign::enginecheck
