#pragma once
// Shared allow/deny state for the bus->event adapter (jlog_adapters.cpp) and the Events viewer panel
// (log_viewer.cpp), so a toggle in the overlay takes effect on the next message without restarting anything.
// Internal to component C: not part of any frozen contract.
#include <string_view>

namespace wf::jlog::busfilter {

// Seeds the allow/deny sets from "a,b,c"-style ini values (see [Logging] EventDeny=/EventAllow=). Call once,
// at Install().
void Init(std::string_view denyList, std::string_view allowList);

// True if a message with this registry name should be turned into a jlog "event" record. Allow wins over deny;
// anything in neither list is logged (matches docs/m0-design.md SS3 "C", adapter 2).
bool ShouldLog(std::string_view name);

bool IsAllowed(std::string_view name);
bool IsDenied(std::string_view name);
void SetAllow(std::string_view name, bool on);
void SetDeny(std::string_view name, bool on);

}  // namespace wf::jlog::busfilter
