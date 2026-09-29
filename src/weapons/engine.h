#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <safetyhook.hpp>

// Engine glue shared by the weapon and asset components. Every call is SEH-guarded and refuses to run when the
// #1077 bytes of the function it calls differ.
namespace melange::weapons::engine {
uintptr_t Drm();
uintptr_t Lookup(const char* name);
uintptr_t LookupDesc(const char* name);
int AddResource(const char* name, uintptr_t obj, uint16_t section, uint32_t flags);  // slot 9; copies obj
int LoadBank(const char* gameRelPath, uint32_t section, uint32_t flags = 0x21);      // slot 42
uintptr_t ClassOf(uintptr_t container);
struct XStr {
    uintptr_t p = 0;
    XStr(const char* s);
    ~XStr();
    XStr(const XStr&) = delete;
    XStr& operator=(const XStr&) = delete;
};
bool AssignXString(uintptr_t field, const char* s);
const char* EnumName(int id);
bool AddSearchPath(const char* gameRelDir);
constexpr uintptr_t kNames = 0x90c920, kPanel = 0x920e58, kSelWrite = 0x603cdb, kSelLog = 0x603cfc,
    kInvGet = 0x67cc33, kAllowed = 0x50c804, kDelay = 0x65a28c, kCanUse = 0x600bd4, kText = 0x600e21, kHelp = 0x5fee4d,
    kHudIcon = 0x5d7f8b, kLaunch = 0x583160, kUpdBase = 0x57fae0, kUpdPara = 0x576fc0, kMsgBase = 0x582860,
    kMsgPara = 0x577f40, kExplode = 0x57f140, kUpload = 0x79dc50;

// Additive helpers.
constexpr int kEnumCount = 0x45;
constexpr uintptr_t kStartCheck = 0x70b3e1;   // host WaitingGameStart: players with a team vs players
bool SitesOk();                               // every function and hook site above has its #1077 bytes
bool SiteIntact(uintptr_t site);              // this hook site still has its #1077 bytes (false for unknown sites)
std::string ReadCString(uintptr_t p, size_t max = 128);
std::string XStringValue(uintptr_t field);    // the text of an XString field, "" if unreadable
std::string ClassName(uintptr_t cls);         // classInfo name ("" if unreadable)
uintptr_t ClassParent(uintptr_t cls);

// Hooks created through these start disabled, are refused unless SiteIntact(site), and are listed by Hooks().
// Enable() records the wanted state; SuppressAll(true) forces every tracked hook off until SuppressAll(false).
bool Mid(SafetyHookMid& h, uintptr_t site, safetyhook::MidHookFn fn, const char* name);
template <class F>
bool Inline(SafetyHookInline& h, uintptr_t site, F fn, const char* name);
bool InlineRaw(SafetyHookInline& h, uintptr_t site, void* fn, const char* name);
void Enable(SafetyHookMid& h, bool on);
void Enable(SafetyHookInline& h, bool on);
void SuppressAll(bool suppress);
bool Suppressed();
struct HookState { std::string name; uintptr_t site; bool created, wanted, enabled; };
std::vector<HookState> Hooks();

template <class F>
bool Inline(SafetyHookInline& h, uintptr_t site, F fn, const char* name) {
    return InlineRaw(h, site, reinterpret_cast<void*>(fn), name);
}
}  // namespace melange::weapons::engine
