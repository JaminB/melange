#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "launcher/setup/engine.h"
#include "update/release.h"

// Melange.exe updating itself: find the latest GitHub release, download and verify it into
// %LOCALAPPDATA%\Melange\updates\<version>\, and later apply it from the new Melange.exe (--apply-update) through the
// setup engine. UI-free; the self-test drives it with file:/// fixtures.
namespace melange::launcher::updater {
// ---------------------------------------------------------------- Authenticode
struct Signer {
    bool present = false;   // the file carries a signature
    bool valid = false;     // WinVerifyTrust accepted it (chain to a trusted root, file unmodified)
    std::string subject;    // leaf certificate subject, X.500 string
    std::string issuerOrg;  // the issuing CA's O=, stable across Azure's rotating intermediates
    std::string thumbprint; // SHA-1, for the log only: short-lived certificates change it every few days
    std::string error;      // why `valid` is false
};
Signer ReadSigner(const std::wstring& file);
// Same publisher: the leaf subject and the issuer organisation match.
bool SameSigner(const Signer& a, const Signer& b);
// "" when `candidate` may replace a binary signed as `running`. An unsigned running exe (a developer build) accepts
// anything; a signed one requires a valid signature by the same publisher.
std::string SignerRefusal(const Signer& running, const Signer& candidate, const std::string& what);

// ---------------------------------------------------------------- download and stage
struct Source {
    std::string latestUrl = update::kLatestUrl;
    std::string downloadPrefix = update::kDownloadPrefix;   // every asset URL must start with this
    std::string userAgent;                                  // "Melange/<version>"
    const std::atomic<bool>* cancel = nullptr;
};
struct Staged {
    std::string version, htmlUrl, sha256;
    std::wstring dir;       // updates\<version>
    std::wstring payload;   // updates\<version>\payload: the release zip, extracted
};
std::wstring Root();   // %LOCALAPPDATA%\Melange\updates

bool FetchLatest(const Source& src, update::Release* out, std::string* err);
// Fetches melange-<v>.json and the zip it names into root\<v>\, checks the zip's length and SHA-256 against the
// manifest (and the release's asset size), extracts it to payload\, checks the payload (VerifyPayload) and writes
// ready.json last. A leftover folder without ready.json is started over.
bool Download(const Source& src, const update::Release& rel, const std::wstring& root, const Signer& running,
              const std::function<void(uint64_t got, uint64_t total)>& progress, Staged* out, std::string* err);
// melange.asi and Melange.exe are this version, Melange.ini and dinput8.dll are there, and both binaries pass
// SignerRefusal against `running`.
bool VerifyPayload(const std::wstring& payload, const std::string& version, const Signer& running, std::string* err);
// Unpacks a release zip: plain relative names only (no drive, '..', backslash, control characters), at most 2000
// entries and 512 MiB, stored or deflate.
bool ExtractZip(const std::wstring& zip, const std::wstring& dir, std::string* err);
// The newest root\<v>\ with ready.json whose version is newer than `current`.
bool FindReady(const std::wstring& root, const std::string& current, Staged* out);
// Deletes every root\<v>\ except `keepVersion` and except one that contains `inUse` (a running exe).
void Clean(const std::wstring& root, const std::string& keepVersion, const std::wstring& inUse);

// ---------------------------------------------------------------- apply
// Melange.exe --apply-update --from <old exe dir> --pid <old pid> [--game <dir>] [--elevated] [--result <file>]
struct ApplyArgs {
    std::wstring from, game, result;
    unsigned long pid = 0;
    bool elevated = false;   // the elevated child: apply and report, never relaunch
};
bool IsApplyCommand(const std::vector<std::wstring>& args);   // args without the program name
bool ParseApplyArgs(const std::vector<std::wstring>& args, ApplyArgs* out, std::string* err);
std::wstring ApplyCommandLine(const std::wstring& exe, const ApplyArgs& a);   // exe "": the arguments alone

struct ApplyOutcome {
    bool ok = false;
    bool needElevation = false;   // access denied: run again as administrator
    bool gameUpdated = false;
    std::string message, backupId;
    std::vector<std::string> warnings;
};
// Runs from the new Melange.exe: the setup engine's install over ctx.gameDir (payloadDir and selfExe are the
// staged ones; skipped when Melange isn't installed there), then the old exe's folder `originDir` gets the new
// Melange.exe (the old one kept as Melange.exe.old until the next start) and any other release file it already had.
ApplyOutcome ApplyStaged(const setup::Context& ctx, const std::wstring& originDir);
// Melange.exe.old beside `dir`'s Melange.exe, left by an earlier apply.
void DeleteOldExe(const std::wstring& dir);

struct Result {
    bool present = false, ok = false, gameUpdated = false;
    std::string version, message, at;
    std::vector<std::string> warnings;
};
std::wstring ResultPath();   // Root()\result.json
bool WriteResult(const std::wstring& path, const Result& r);
// Reads and deletes it: the next launcher start shows it once.
bool TakeResult(const std::wstring& path, Result* out);
std::string ResultJson(const Result& r);

// ---------------------------------------------------------------- the in-game check
// Makes gameDir\Melange.ini's [Update] CheckInGame agree with Settings › Updates (1/0), the way melange.asi reads it
// (missing or empty is on). Rewrites the file only when it disagrees, keeping its encoding and every other byte.
// No Melange.ini (no game folder, or Melange not installed): nothing to do. False and *err when it can't be written.
bool SyncInGameCheck(const std::wstring& gameDir, bool on, bool* changed, std::string* err);
}  // namespace melange::launcher::updater
