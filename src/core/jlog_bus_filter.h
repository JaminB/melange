#pragma once
// Allow/deny state shared by the bus->event log adapter and the overlay's Events panel; toggles apply immediately.
#include <string_view>

namespace melange::jlog::busfilter {

// Seeds the sets from the comma-separated [Logging] EventDeny/EventAllow values. Call once.
void Init(std::string_view denyList, std::string_view allowList);

// Allow wins over deny; a name in neither list is logged.
bool ShouldLog(std::string_view name);

bool IsAllowed(std::string_view name);
bool IsDenied(std::string_view name);
void SetAllow(std::string_view name, bool on);
void SetDeny(std::string_view name, bool on);

}  // namespace melange::jlog::busfilter
