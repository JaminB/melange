#pragma once
#include <string_view>
namespace melange::testcmd {
// Handler for an Automation command line "<verb> <args>". Runs on the main thread (inside the Frame event).
// Return false to log "[auto] <verb> failed". Handlers must not block for more than a frame.
using Handler = bool (*)(std::string_view args, void* user);
bool Register(const char* verb, Handler fn, void* user = nullptr);  // verbs are case-insensitive, unique
// Called by Automation for verbs it does not know itself. Returns false if no handler exists.
bool Dispatch(std::string_view verb, std::string_view args);
}
