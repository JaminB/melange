#pragma once
// The helper's cache folder lifecycle (design 1.1.4, Critique T2). Core gives every helper instance its own folder
// (--cache, media-cache\<pid>-<rand>\ under LocalLow); the helper holds <cache>\lock open for its whole life, so two
// game instances on one machine (LocalNet A+B) never delete each other's data. Compiled into every build, with or
// without torrent support (the folder also holds mpv's and deno's scratch files).
//
// Call order in the helper's main (C6a): HoldInstanceLock first; SweepInstances only with --sweep (core passes it on
// the first launch per game process, never on a watchdog restart). The libtorrent engine calls SweepTorrents itself
// when it is created.
#include <string>

namespace holo::bt::cache {
// Creates <cacheDir> if needed and opens <cacheDir>\lock without delete sharing; the handle stays open until the
// process exits. False with a reason when the folder or the lock cannot be created (the helper should then quit:
// a folder without a held lock may be swept by another instance).
bool HoldInstanceLock(const std::wstring& cacheDir, std::string* err);

// Deletes the sibling instance folders of <cacheDir> whose owner is gone: those whose lock file can be opened for
// delete, and those with no lock file that are older than 10 minutes (core creates the folder just before the
// launch, so a young folder without a lock belongs to a helper that is still starting). Only names of the form
// "<pid>-<rand>" are touched, and never <cacheDir> itself. Returns the number of folders deleted.
int SweepInstances(const std::wstring& cacheDir);

// Deletes the torrent folders <cacheDir>\bt\<infohash> left by an earlier run of this instance (a crash, or a delete
// that failed while a file was still open). Returns the number deleted.
int SweepTorrents(const std::wstring& cacheDir);

// Deletes a folder and everything under it. Reparse points (junctions, symlinks) are removed as links and never
// followed, so a link planted in the cache cannot make the helper delete files outside it. False if anything is left.
bool DeleteTree(const std::wstring& dir);

// <cacheDir>\bt\<ih> (the save path of one torrent).
std::wstring TorrentDir(const std::wstring& cacheDir, const std::string& ih);
}  // namespace holo::bt::cache
