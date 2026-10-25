// The media pack installer and the yt-dlp lane (pack.h).
// Placeholder from Melange 0.12.0 C0: the pack is always missing and installs are refused. C4a replaces this file.
#include "media/pack.h"

#include "media/pack_manifest.h"

namespace melange::media::pack {
const char* StateName(State s) {
    switch (s) {
        case State::Missing: return "missing";
        case State::Downloading: return "downloading";
        case State::Verifying: return "verifying";
        case State::Installing: return "installing";
        case State::Ready: return "ready";
        case State::Updating: return "updating";
        case State::Error: return "error";
        case State::Unsupported: return "unsupported";
        case State::Disabled: return "disabled";
    }
    return "?";
}

Info Get() {
    Info i;
    i.state = State::Missing;
    i.version = manifest::kPackVersion;
    i.downloadMB = manifest::kDownloadMB;
    i.diskMB = manifest::kDiskMB;
    return i;
}

bool Install(const char*, std::string* reason) {
    if (reason) *reason = "unavailable";
    return false;
}
void CancelInstall() {}
bool ReadyPaths(Paths*) { return false; }
bool VerifyForLaunch(const Paths&, Held*, std::string* err) {
    if (err) *err = "unavailable";
    return false;
}
void ReleaseHeld(Held* h) {
    if (h) h->handles.clear();
}
void CheckYtdlpLane(bool) {}
}  // namespace melange::media::pack
