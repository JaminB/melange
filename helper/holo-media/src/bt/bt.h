#pragma once
// The helper's torrent engine, behind one interface so the player side (C6a: main loop, mpv, pipe) and the engine
// (C6b: libtorrent 2.x, or the anacrolix/torrent sidecar fallback) are built independently. Written for Melange 0.12.0
// C0 and frozen: a change needs both owners.
//
// Threading: every Engine method is called from the helper's main (pipe) thread. Stream::Read is called from mpv's
// demuxer thread and may block; Stream::Cancel is called from any thread and must unblock a Read in progress.
// Rules the engine enforces itself (docs/media-helper.md "Torrents"): magnets arrive canonical (url_logic.h) and are
// re-checked; every file starts at priority 0; url seeds and http seeds from the metadata are removed;
// ssrf_mitigation on, allow_idna off, validate_https_trackers on; trackers resolving to loopback, link-local or
// RFC 1918 addresses are dropped; posix disk I/O; data only under Settings::cacheDir\bt\<infohash>.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace holo::bt {
struct Settings {
    std::wstring cacheDir;   // this helper instance's folder (--cache); torrents go to <cacheDir>\bt\<infohash>
    int uploadKBps = 1024;   // [Media] TorrentUploadKBps (from bt.add; the latest value wins)
    bool upnp = false;       // [Media] TorrentUPnP: UPnP and NAT-PMP
    int port = 0;            // [Media] TorrentPort, 0 = random
};

struct FileInfo {
    int index = 0;
    std::string path;    // UTF-8, as in the metadata (relative, '/' separated)
    uint64_t size = 0;
    bool video = false;  // url::IsVideoFile(path) and not url::IsRefusedFile(path)
};

struct Status {
    double down = 0, up = 0;  // bytes per second
    int peers = 0;
    double progress = 0;      // of the selected file, 0..1 (0 before a select)
    uint64_t have = 0;        // bytes of the selected file on disk
    bool metadata = false;
};

// What Poll reports; the helper forwards each one as the pipe message of the same name.
struct Event {
    enum class Kind : uint8_t { Files, Status, Removed, Error };
    Kind kind = Kind::Status;
    std::string ih;                // lowercase hex infohash
    std::string name;              // Files: the torrent's name
    std::vector<FileInfo> files;   // Files
    Status status;                 // Status (at most once per second per torrent)
    std::string code, message;     // Error: proto::err codes (kTorrent, kDisk)
};

// A read-only byte stream over one selected file, for mpv_stream_cb_add_ro. Pieces are fetched with deadlines
// around the read position (sequential download, the head, the last 2 MB for an MP4 moov, a read-ahead window).
class Stream {
public:
    virtual ~Stream() = default;
    virtual int64_t Size() = 0;                       // bytes
    virtual int64_t Read(void* buf, uint64_t n) = 0;  // bytes read, 0 at the end, -1 on error or after Cancel
    virtual bool Seek(int64_t offset) = 0;
    virtual void Cancel() = 0;                        // unblocks a Read; the stream is useless afterwards
};

class Engine {
public:
    virtual ~Engine() = default;
    // Adds a torrent from its canonical magnet with every file at priority 0. False with a reason on failure.
    virtual bool Add(const std::string& ih, const std::string& magnet, const Settings& s, std::string* err) = 0;
    // Downloads one file (priority 7, sequential); the previous selection of that torrent goes back to 0.
    virtual bool Select(const std::string& ih, int index, std::string* err) = 0;
    // Closes the torrent's streams, removes it with its files and deletes its folder (retried at the next start).
    // Reports Event::Removed once done (or after 5 s waiting for the deletion).
    virtual void Remove(const std::string& ih) = 0;
    // A stream over a selected file; nullptr with a reason if the torrent or file is unknown.
    virtual std::unique_ptr<Stream> Open(const std::string& ih, int index, std::string* err) = 0;
    // Drains alerts into events. Called every main-loop turn (at least 10 times a second).
    virtual void Poll(std::vector<Event>* out) = 0;
    // Removes every torrent; called on quit and on parent death. Must return within 5 s.
    virtual void Shutdown() = 0;
    virtual std::string Version() const = 0;  // "libtorrent 2.0.x", or "" without torrent support
};

// The engine of this build (bt_libtorrent*.cpp with HOLO_TORRENT=ON, bt_none.cpp otherwise). nullptr with a reason
// when torrents are unavailable; the helper then answers every bt.* message with error{code="torrent"}.
std::unique_ptr<Engine> CreateEngine(const Settings& s, std::string* err);

// mpv opens a torrent file through this protocol: "holobt://<infohash>/<index>".
inline constexpr const char kStreamProtocol[] = "holobt";
inline std::string StreamUrl(const std::string& ih, int index) { return std::string(kStreamProtocol) + "://" + ih + "/" + std::to_string(index); }
}  // namespace holo::bt
