#pragma once
// Internal to component C: the Logging module (jlog_adapters.cpp) calls this once, after the overlay's own
// module has had a chance to install, to register the "Log" and "Events" panels. Not a frozen contract.
namespace melange::logviewer {
void Install();
}
