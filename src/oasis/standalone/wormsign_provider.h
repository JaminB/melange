#pragma once
#include <string>

// File-backed wormsign.library for oasis.exe: every *.wsr under the replays directory, read with the same
// wsr::Reader the game and the recorder use (src/wormsign/format.h has no game dependency). No live session, so
// arm/control/pin/detail are not offered here -- only list and read (the caller still needs the /replays/ route
// for downloads, exactly like /captures/ and /logs/).
namespace melange::oasis::standalone::wormsignprov {
std::string ListJson(const std::wstring& replaysDir);
}  // namespace melange::oasis::standalone::wormsignprov
