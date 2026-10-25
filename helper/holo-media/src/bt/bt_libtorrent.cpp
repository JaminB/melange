// The torrent engine on libtorrent 2.x (vcpkg x64-windows-static, built with HOLO_TORRENT=ON). docs/media-helper.md
// "Torrents" and design 1.1.4: magnets are re-classified and added by infohash with every file at priority 0; the
// chosen file downloads sequentially with deadlines on its head, its last 2 MB and a read-ahead window around the
// stream position; mpv reads the finished pieces straight from the file (Stream below). Hardening (Critique S5):
// only the canonical magnet's xt, dn and tr are used, trackers whose host resolves to a loopback, link-local or
// private address are never added, url and http seeds from the metadata are removed, ssrf_mitigation is on, IDNA is
// off, https trackers are validated, symlinked files are never offered. The session (and so the listen socket that
// raises the Windows Firewall prompt, S6) starts with the first Add, never at helper start.
//
// Threading: Add/Select/Remove/Poll/Shutdown run on the helper's main thread. Open is also safe from mpv's threads
// (the stream_cb bridge in bt_mpv.cpp calls it from mpv's open callback); everything shared sits under mu_. A
// Stream's Read blocks on its own mutex and condition variable and never takes mu_.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/address.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/announce_entry.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>

#include "bt/bt.h"
#include "bt/bt_cache.h"
#include "bt/bt_libtorrent.h"
#include "bt/bt_logic.h"
#include "media/url_logic.h"

namespace holo::bt {
namespace {
namespace url = melange::media::url;
namespace err = melange::media::proto::err;
using Clock = std::chrono::steady_clock;

constexpr int kMaxTorrents = 16;          // the core allows 4 per mod; this is the helper-wide ceiling
constexpr int kRemoveWaitMs = 5000;       // bt.h: Removed after the deletion, or after 5 s
constexpr int kShutdownDeleteMs = 2500;   // Shutdown: waiting for deletions, then the session teardown
constexpr int kShutdownAbortMs = 2000;
constexpr int kStatusMs = 1000;
constexpr int kErrorRepeatMs = 5000;
std::atomic<bool> g_loopbackOnly{false};  // testing::LoopbackOnly

int64_t MsSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool HexBytes(const std::string& hex, char* out, size_t n) {
    if (hex.size() != n * 2) return false;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<char>(nib(hex[2 * i]) * 16 + nib(hex[2 * i + 1]));
    return true;
}

// Lowercase hex of a digest, the form bt.* messages use.
template <class Digest>
std::string Hex(const Digest& d) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    for (const char c : d) {
        s.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 15]);
        s.push_back(kHex[static_cast<unsigned char>(c) & 15]);
    }
    return s;
}

// Disk full shows up as a torrent error as well as a file error; core shows both as "disk".
bool DiskFull(const lt::error_code& ec) {
    const int v = ec.value();
    return v == ERROR_DISK_FULL || v == ERROR_HANDLE_DISK_FULL || (ec.category() == boost::system::generic_category() && v == ENOSPC);
}

// True when a full path stays inside `dir` (no "..", no other root): libtorrent sanitises metadata paths already,
// this is the second check before the helper opens anything a peer named.
bool Inside(const std::wstring& dir, const std::wstring& path) {
    if (path.size() <= dir.size() + 1 || _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
    if (path[dir.size()] != L'\\' && path[dir.size()] != L'/') return false;
    std::wstring rest = path.substr(dir.size() + 1);
    size_t start = 0;
    while (start <= rest.size()) {
        const size_t end = rest.find_first_of(L"\\/", start);
        const std::wstring part = rest.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L".." || part.find(L':') != std::wstring::npos) return false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}

// Creates the selected file (and its folders) as an NTFS sparse file before libtorrent writes to it. libtorrent's
// POSIX storage writes at offsets, and writing the last 2 MB of a plain 4 GB file first would make NTFS zero-fill
// everything before it.
void PrepareSparse(const std::wstring& path) {
    for (size_t i = 3; i < path.size(); ++i)
        if (path[i] == L'\\' || path[i] == L'/') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &got, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------- tracker vetting (S5)
// True when every address the tracker's host resolves to is public. Runs on a worker thread (DNS can take seconds).
// libtorrent resolves the name again when it announces, so a rebinding host could still answer differently; for
// that, ssrf_mitigation limits trackers on the local network to the /announce path.
bool TrackerAllowed(const std::string& tr) {
    static const bool wsa = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;  // once per process, never cleaned up (no teardown at exit)
    }();
    if (!wsa) return false;
    std::string host;
    int port = 0;
    if (!logic::TrackerHost(tr, &host, &port)) return false;
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfoW* res = nullptr;
    const std::wstring wh = Wide(host);
    const bool literal = GetAddrInfoW(wh.c_str(), nullptr, &hints, &res) == 0;
    if (!literal) {
        if (res) FreeAddrInfoW(res);
        res = nullptr;
        if (logic::LocalHostName(host)) return false;
        hints.ai_flags = 0;
        if (GetAddrInfoW(wh.c_str(), nullptr, &hints, &res) != 0) return false;
    }
    bool any = false, ok = true;
    for (addrinfoW* a = res; a; a = a->ai_next) {
        if (a->ai_family == AF_INET) {
            any = true;
            ok = ok && logic::PublicV4(reinterpret_cast<const uint8_t*>(&reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr));
        } else if (a->ai_family == AF_INET6) {
            any = true;
            ok = ok && logic::PublicV6(reinterpret_cast<const uint8_t*>(&reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr));
        }
    }
    if (res) FreeAddrInfoW(res);
    return any && ok;
}

// Vetted trackers wait here until Poll adds them on the main thread.
struct TrackerQueue {
    std::mutex m;
    std::vector<std::pair<std::string, std::string>> ready;  // ih, tracker url
};

// ---------------------------------------------------------------- streams
struct StreamCore {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> cancelled{false};
    lt::torrent_handle h;
    logic::Span span;
    std::wstring path;
    int index = 0;
    HANDLE file = INVALID_HANDLE_VALUE;  // under m
    int64_t pos = 0;                     // under m
    int windowHead = -1;                 // under m: the piece the deadlines were last set from
    std::vector<uint8_t> have;           // under m: pieces of the file known to be complete
    double rate = 0;                     // nominal bytes per second (logic::NominalRate)

    ~StreamCore() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    void Wake() {
        std::lock_guard<std::mutex> lock(m);
        cv.notify_all();
    }
    void Cancel() {
        cancelled = true;
        std::lock_guard<std::mutex> lock(m);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);  // so the torrent's folder can go while mpv still holds the stream
            file = INVALID_HANDLE_VALUE;
        }
        cv.notify_all();
    }
};

class LtStream final : public Stream {
public:
    explicit LtStream(std::shared_ptr<StreamCore> c) : c_(std::move(c)) {}
    ~LtStream() override { c_->Cancel(); }
    int64_t Size() override { return c_->span.size; }
    bool Seek(int64_t offset) override {
        std::lock_guard<std::mutex> lock(c_->m);
        if (c_->cancelled || offset < 0 || offset > c_->span.size) return false;
        const int piece = c_->span.PieceAt(offset);
        const int w = logic::WindowPieces(c_->span.pieceLen, c_->rate);
        if (c_->windowHead >= 0 && (piece < c_->windowHead || piece >= c_->windowHead + w)) {
            // A jump out of the window: the old deadlines would compete with the new position.
            try {
                c_->h.clear_piece_deadlines();
            } catch (...) {
            }
            c_->windowHead = -1;
        }
        c_->pos = offset;
        return true;
    }
    void Cancel() override { c_->Cancel(); }
    int64_t Read(void* buf, uint64_t n) override;

private:
    bool HavePiece(std::unique_lock<std::mutex>& lock, int piece);
    void SetWindow(int piece);
    std::shared_ptr<StreamCore> c_;
};

// Called with c_->m held; drops it around the libtorrent call (a round trip to the network thread), so Cancel never
// waits on it.
bool LtStream::HavePiece(std::unique_lock<std::mutex>& lock, int piece) {
    const int rel = piece - c_->span.First();
    if (rel >= 0 && rel < static_cast<int>(c_->have.size()) && c_->have[rel]) return true;
    lt::torrent_handle h = c_->h;
    lock.unlock();
    bool have = false;
    try {
        have = h.is_valid() && h.have_piece(lt::piece_index_t(piece));
    } catch (...) {
        have = false;
    }
    lock.lock();
    if (have && rel >= 0 && rel < static_cast<int>(c_->have.size())) c_->have[rel] = 1;
    return have;
}

void LtStream::SetWindow(int piece) {
    if (piece == c_->windowHead) return;
    c_->windowHead = piece;
    const int w = logic::WindowPieces(c_->span.pieceLen, c_->rate);
    const int last = c_->span.Last(), first = c_->span.First();
    try {
        for (int k = 0; k < w && piece + k <= last; ++k) {
            const int rel = piece + k - first;
            if (rel >= 0 && rel < static_cast<int>(c_->have.size()) && c_->have[rel]) continue;
            c_->h.set_piece_deadline(lt::piece_index_t(piece + k), logic::DeadlineMs(k, c_->span.pieceLen, c_->rate));
        }
    } catch (...) {
    }
}

int64_t LtStream::Read(void* buf, uint64_t n) {
    std::unique_lock<std::mutex> lock(c_->m);
    if (c_->cancelled) return -1;
    if (c_->pos >= c_->span.size) return 0;
    if (n == 0) return 0;
    const int piece = c_->span.PieceAt(c_->pos);
    SetWindow(piece);
    while (!HavePiece(lock, piece)) {
        if (c_->cancelled) return -1;
        try {
            if (!c_->h.is_valid()) return -1;
        } catch (...) {
            return -1;
        }
        c_->cv.wait_for(lock, std::chrono::milliseconds(250));  // woken by Poll on piece_finished_alert
        if (c_->cancelled) return -1;
    }
    if (c_->cancelled) return -1;  // HavePiece let go of the lock: Cancel may have run meanwhile
    if (c_->file == INVALID_HANDLE_VALUE) {
        HANDLE f = CreateFileW(c_->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (f == INVALID_HANDLE_VALUE) return -1;
        BY_HANDLE_FILE_INFORMATION fi{};
        if (!GetFileInformationByHandle(f, &fi) || (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            CloseHandle(f);
            return -1;  // never read through a link a torrent planted
        }
        c_->file = f;
    }
    int64_t want = c_->span.ToPieceEnd(c_->pos);
    if (static_cast<uint64_t>(want) > n) want = static_cast<int64_t>(n);
    if (want > (1 << 24)) want = 1 << 24;
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(c_->pos & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(c_->pos >> 32);
    DWORD got = 0;
    if (!ReadFile(c_->file, buf, static_cast<DWORD>(want), &got, &ov) || got == 0) return -1;
    c_->pos += got;
    return got;
}

// ---------------------------------------------------------------- torrents
struct FileMeta {
    std::string path;      // metadata path, '/' separated (bt.files)
    std::wstring full;     // on disk, inside the torrent's folder
    logic::Span span;
    bool pad = false;  // BEP 47 padding, never listed
    bool playable = false;
};

struct Torrent {
    std::string ih;
    lt::torrent_handle h;
    std::wstring dir;
    std::string name;
    bool metadata = false, filesSent = false;
    std::vector<FileMeta> files;  // by libtorrent file index (pad files included, never playable)
    int selected = -1, pendingSelect = -1;
    bool removing = false, deleted = false;
    Clock::time_point removeAt, lastStatus, lastError;
    std::vector<std::weak_ptr<StreamCore>> streams;

    // Every stream of the torrent, or (keep >= 0) those over other files than `keep`.
    void CancelStreams(int keep = -1) {
        for (auto& w : streams)
            if (auto c = w.lock())
                if (keep < 0 || c->index != keep) c->Cancel();
    }
    void WakeStreams() {
        streams.erase(std::remove_if(streams.begin(), streams.end(), [](auto& w) { return w.expired(); }), streams.end());
        for (auto& w : streams)
            if (auto c = w.lock()) c->Wake();
    }
};

class LtEngine final : public Engine {
public:
    explicit LtEngine(const Settings& s) : settings_(s), trackers_(std::make_shared<TrackerQueue>()) {}
    ~LtEngine() override { Shutdown(); }

    bool Add(const std::string& ih, const std::string& magnet, const Settings& s, std::string* err) override;
    bool Select(const std::string& ih, int index, std::string* err) override;
    void Remove(const std::string& ih) override;
    std::unique_ptr<Stream> Open(const std::string& ih, int index, std::string* err) override;
    void Poll(std::vector<Event>* out) override;
    void Shutdown() override;
    std::string Version() const override { return std::string("libtorrent ") + lt::version(); }

    bool TestConnectPeer(const std::string& ih, const std::string& ip, int port);
    int TestCount(const std::string& ih, bool trackers);

private:
    lt::settings_pack Pack(const Settings& s) const;
    bool StartSession(std::string* err);
    void OnMetadata(Torrent& t, std::vector<Event>* out);
    bool ApplySelect(Torrent& t, int index, std::string* err);
    Torrent* Find(const std::string& ih);
    Torrent* FindByHashes(const lt::info_hash_t& h);
    void Fail(Torrent& t, const char* code, const std::string& message, std::vector<Event>* out);

    std::mutex mu_;
    Settings settings_;
    std::unique_ptr<lt::session> ses_;
    std::map<std::string, std::unique_ptr<Torrent>> torrents_;
    std::vector<Event> pending_;  // events produced outside Poll (Remove of an unknown torrent)
    std::shared_ptr<TrackerQueue> trackers_;
    bool shutdown_ = false;
};

lt::settings_pack LtEngine::Pack(const Settings& s) const {
    lt::settings_pack p;
    p.set_int(lt::settings_pack::alert_mask, lt::alert_category::status | lt::alert_category::error | lt::alert_category::storage |
                                                 lt::alert_category::piece_progress);
    const std::string port = std::to_string(s.port > 0 && s.port <= 65535 ? s.port : 0);
    const bool loop = g_loopbackOnly;
    p.set_str(lt::settings_pack::listen_interfaces, loop ? "127.0.0.1:0" : "0.0.0.0:" + port + ",[::]:" + port);
    p.set_bool(lt::settings_pack::enable_dht, !loop);
    p.set_bool(lt::settings_pack::enable_lsd, !loop);
    p.set_bool(lt::settings_pack::enable_upnp, s.upnp);
    p.set_bool(lt::settings_pack::enable_natpmp, s.upnp);
    p.set_bool(lt::settings_pack::ssrf_mitigation, true);
    p.set_bool(lt::settings_pack::allow_idna, false);
    p.set_bool(lt::settings_pack::validate_https_trackers, true);
    p.set_bool(lt::settings_pack::anonymous_mode, false);
    const int kbps = s.uploadKBps < 1 ? 1 : (s.uploadKBps > 1048576 ? 1048576 : s.uploadKBps);
    p.set_int(lt::settings_pack::upload_rate_limit, kbps * 1024);
    p.set_int(lt::settings_pack::connections_limit, 200);
    p.set_int(lt::settings_pack::stop_tracker_timeout, 1);
    return p;
}

bool LtEngine::StartSession(std::string* err) {
    if (ses_) return true;
    try {
        lt::session_params params(Pack(settings_));
        params.disk_io_constructor = lt::posix_disk_io_constructor;  // no mmap working set (Critique T6)
        ses_ = std::make_unique<lt::session>(std::move(params));
        return true;
    } catch (const std::exception& e) {
        if (err) *err = std::string("libtorrent session: ") + e.what();
        return false;
    }
}

Torrent* LtEngine::Find(const std::string& ih) {
    auto it = torrents_.find(ih);
    return it == torrents_.end() ? nullptr : it->second.get();
}
Torrent* LtEngine::FindByHashes(const lt::info_hash_t& h) {
    if (h.has_v1())
        if (Torrent* t = Find(Hex(h.v1))) return t;
    if (h.has_v2())
        if (Torrent* t = Find(Hex(h.v2))) return t;
    return nullptr;
}

void LtEngine::Fail(Torrent& t, const char* code, const std::string& message, std::vector<Event>* out) {
    if (MsSince(t.lastError) < kErrorRepeatMs) return;
    t.lastError = Clock::now();
    Event e;
    e.kind = Event::Kind::Error;
    e.ih = t.ih;
    e.code = code;
    e.message = url::CleanName(message, melange::media::proto::kMaxMessageBytes);
    out->push_back(std::move(e));
}

bool LtEngine::Add(const std::string& ih, const std::string& magnet, const Settings& s, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutdown_) {
        if (err) *err = "shutting down";
        return false;
    }
    // Re-check what core sent: the magnet must classify to this very infohash; only its canonical parts are used.
    url::Classified cl;
    if (!url::ValidInfohash(ih) || !url::Classify(magnet, &cl) || cl.kind != url::Kind::Magnet || cl.bt.infohash != ih ||
        cl.url != magnet) {
        if (err) *err = "not a canonical magnet for this infohash";
        return false;
    }
    const bool changed = s.uploadKBps != settings_.uploadKBps || s.upnp != settings_.upnp || s.port != settings_.port;
    settings_.uploadKBps = s.uploadKBps;
    settings_.upnp = s.upnp;
    settings_.port = s.port;
    if (!ses_) {
        if (!StartSession(err)) return false;
    } else if (changed) {
        try {
            ses_->apply_settings(Pack(settings_));
        } catch (...) {
        }
    }
    if (Torrent* t = Find(ih)) {
        if (t->removing) {
            if (err) *err = "still being removed";
            return false;
        }
        t->filesSent = false;  // a repeated add (core restarted the item) gets the file list again
        return true;
    }
    if (static_cast<int>(torrents_.size()) >= kMaxTorrents) {
        if (err) *err = "too many torrents";
        return false;
    }

    auto t = std::make_unique<Torrent>();
    t->ih = ih;
    t->dir = cache::TorrentDir(settings_.cacheDir, ih);
    t->name = cl.bt.name;
    CreateDirectoryW(t->dir.substr(0, t->dir.size() - ih.size() - 1).c_str(), nullptr);  // <cache>t
    if (!CreateDirectoryW(t->dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (err) *err = "cannot create the torrent folder (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    lt::add_torrent_params atp;
    if (cl.bt.v2()) {
        char b[32];
        HexBytes(ih, b, 32);
        atp.info_hashes.v2 = lt::sha256_hash(b);
    } else {
        char b[20];
        HexBytes(ih, b, 20);
        atp.info_hashes.v1 = lt::sha1_hash(b);
    }
    if (!cl.bt.name.empty()) atp.name = cl.bt.name;
    atp.save_path = Utf8(t->dir);
    // Not queued behind other torrents, and nothing downloads before a file is chosen.
    atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
    atp.flags |= lt::torrent_flags::default_dont_download;
    lt::error_code ec;
    t->h = ses_->add_torrent(std::move(atp), ec);
    if (ec || !t->h.is_valid()) {
        if (err) *err = "libtorrent add: " + ec.message();
        return false;
    }
    t->lastStatus = Clock::now();
    t->lastError = Clock::now() - std::chrono::hours(1);

    // Trackers join only after their hosts were checked off the main thread (S5). DHT and LSD find peers meanwhile.
    if (!cl.bt.trackers.empty()) {
        std::thread([q = trackers_, ih, list = cl.bt.trackers] {
            for (const std::string& tr : list)
                if (TrackerAllowed(tr)) {
                    std::lock_guard<std::mutex> g(q->m);
                    q->ready.emplace_back(ih, tr);
                }
        }).detach();
    }
    torrents_[ih] = std::move(t);
    return true;
}

void LtEngine::OnMetadata(Torrent& t, std::vector<Event>* out) {
    if (t.metadata) return;
    std::shared_ptr<const lt::torrent_info> ti;
    try {
        // Web seeds would make the helper fetch whatever URL the torrent's author chose (S5). A magnet's metadata is
        // only the info dictionary, which carries none in libtorrent 2.1 (url-list sits outside it), and the torrent
        // never contacts one while every file is at priority 0 ("finished"); removing them here keeps it that way
        // whatever a later libtorrent reads. BEP 17 http seeds are gone from 2.1's ABI.
        for (const std::string& u : t.h.url_seeds()) t.h.remove_url_seed(u);
        ti = t.h.torrent_file();
    } catch (...) {
    }
    if (!ti) return;
    t.metadata = true;
    const lt::file_storage& fs = ti->layout();
    if (!ti->name().empty()) t.name = url::CleanName(ti->name(), melange::media::proto::kMaxNameBytes);
    t.files.clear();
    t.files.resize(static_cast<size_t>(fs.num_files()));
    const std::string save = Utf8(t.dir);
    for (const lt::file_index_t i : fs.file_range()) {
        FileMeta& f = t.files[static_cast<size_t>(static_cast<int>(i))];
        f.path = logic::CleanPath(fs.file_path(i));
        f.span.offset = fs.file_offset(i);
        f.span.size = fs.file_size(i);
        f.span.pieceLen = fs.piece_length();
        f.span.numPieces = fs.num_pieces();
        f.full = Wide(fs.file_path(i, save));
        for (wchar_t& c : f.full)
            if (c == L'/') c = L'\\';
        const bool symlink = bool(fs.file_flags(i) & lt::file_storage::flag_symlink);
        f.pad = fs.pad_file_at(i);
        f.playable = logic::PlayableFile(f.path, symlink, f.pad) && Inside(t.dir, f.full) && f.span.size > 0 &&
                     static_cast<int>(i) < static_cast<int>(melange::media::proto::kMaxTorrentFiles);
    }
    // Every file stays at priority 0 (default_dont_download already did this; set it again in case).
    try {
        t.h.prioritize_files(std::vector<lt::download_priority_t>(t.files.size(), lt::dont_download));
    } catch (...) {
    }
    if (t.pendingSelect >= 0) {
        std::string why;
        const int idx = t.pendingSelect;
        t.pendingSelect = -1;
        if (!ApplySelect(t, idx, &why)) Fail(t, err::kTorrent, why, out);
    }
}

bool LtEngine::ApplySelect(Torrent& t, int index, std::string* err) {
    if (index < 0 || index >= static_cast<int>(t.files.size())) {
        if (err) *err = "no such file";
        return false;
    }
    FileMeta& f = t.files[static_cast<size_t>(index)];
    if (!f.playable) {
        if (err) *err = "not a video file";
        return false;
    }
    if (t.selected == index) return true;
    t.CancelStreams(index);
    PrepareSparse(f.full);
    try {
        std::vector<lt::download_priority_t> prio(t.files.size(), lt::dont_download);
        prio[static_cast<size_t>(index)] = lt::top_priority;
        t.h.prioritize_files(prio);
        t.h.set_flags(lt::torrent_flags::sequential_download);
        t.h.clear_piece_deadlines();
        // The head first, then the tail (an MP4's moov, an MKV's cues), so mpv can open and seek before the middle.
        const logic::Span& s = f.span;
        const int headLast = s.PieceAt(std::min<int64_t>(logic::kHeadBytes, s.size) - 1);
        int k = 0;
        for (int p = s.First(); p <= headLast; ++p, ++k) t.h.set_piece_deadline(lt::piece_index_t(p), k * 100);
        const int tailFirst = s.PieceAt(s.size > logic::kTailBytes ? s.size - logic::kTailBytes : 0);
        for (int p = std::max(tailFirst, headLast + 1); p <= s.Last(); ++p, ++k)
            t.h.set_piece_deadline(lt::piece_index_t(p), 300 + k * 100);
    } catch (const std::exception& e) {
        if (err) *err = std::string("libtorrent select: ") + e.what();
        return false;
    }
    t.selected = index;
    return true;
}

bool LtEngine::Select(const std::string& ih, int index, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    Torrent* t = Find(ih);
    if (!t || t->removing) {
        if (err) *err = "unknown torrent";
        return false;
    }
    if (!t->metadata) {
        t->pendingSelect = index;  // applied (and checked) when the metadata arrives
        return true;
    }
    return ApplySelect(*t, index, err);
}

void LtEngine::Remove(const std::string& ih) {
    std::lock_guard<std::mutex> lock(mu_);
    Torrent* t = Find(ih);
    if (!t) {
        Event e;
        e.kind = Event::Kind::Removed;
        e.ih = ih;
        pending_.push_back(std::move(e));  // nothing to remove: still answer, so core's state settles
        return;
    }
    if (t->removing) return;
    t->CancelStreams();  // mpv's reads end first, then the files go
    t->removing = true;
    t->removeAt = Clock::now();
    try {
        if (ses_ && t->h.is_valid()) ses_->remove_torrent(t->h, lt::session::delete_files);
        else t->deleted = true;
    } catch (...) {
        t->deleted = true;
    }
}

std::unique_ptr<Stream> LtEngine::Open(const std::string& ih, int index, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    Torrent* t = Find(ih);
    if (!t || t->removing) {
        if (err) *err = "unknown torrent";
        return nullptr;
    }
    if (!t->metadata) {
        if (err) *err = "no metadata yet";
        return nullptr;
    }
    if (!ApplySelect(*t, index, err)) return nullptr;  // a no-op for the selected file
    const FileMeta& f = t->files[static_cast<size_t>(index)];
    auto c = std::make_shared<StreamCore>();
    c->h = t->h;
    c->span = f.span;
    c->path = f.full;
    c->index = index;
    c->rate = logic::NominalRate(f.span.size);
    c->have.assign(static_cast<size_t>(f.span.Last() - f.span.First() + 1), 0);
    t->streams.push_back(c);
    return std::make_unique<LtStream>(std::move(c));
}

void LtEngine::Poll(std::vector<Event>* out) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Event& e : pending_) out->push_back(std::move(e));
    pending_.clear();
    if (!ses_) return;

    {
        std::vector<std::pair<std::string, std::string>> ready;
        {
            std::lock_guard<std::mutex> g(trackers_->m);
            ready.swap(trackers_->ready);
        }
        for (auto& [ih, tr] : ready)
            if (Torrent* t = Find(ih); t && !t->removing) try {
                    t->h.add_tracker(lt::announce_entry(tr));
                } catch (...) {
                }
    }

    std::vector<lt::alert*> alerts;
    ses_->pop_alerts(&alerts);
    for (lt::alert* a : alerts) {
        if (auto* m = lt::alert_cast<lt::metadata_received_alert>(a)) {
            if (Torrent* t = FindByHashes(m->handle.info_hashes()); t && !t->removing) OnMetadata(*t, out);
        } else if (auto* p = lt::alert_cast<lt::piece_finished_alert>(a)) {
            if (Torrent* t = FindByHashes(p->handle.info_hashes())) t->WakeStreams();
        } else if (auto* d = lt::alert_cast<lt::torrent_deleted_alert>(a)) {
            if (Torrent* t = FindByHashes(d->info_hashes)) t->deleted = true;
        } else if (auto* df = lt::alert_cast<lt::torrent_delete_failed_alert>(a)) {
            if (Torrent* t = FindByHashes(df->info_hashes)) t->deleted = true;  // the folder delete below retries
        } else if (auto* fe = lt::alert_cast<lt::file_error_alert>(a)) {
            if (Torrent* t = FindByHashes(fe->handle.info_hashes()); t && !t->removing) Fail(*t, err::kDisk, fe->error.message(), out);
        } else if (auto* te = lt::alert_cast<lt::torrent_error_alert>(a)) {
            if (Torrent* t = FindByHashes(te->handle.info_hashes()); t && !t->removing)
                Fail(*t, DiskFull(te->error) ? err::kDisk : err::kTorrent, te->error.message(), out);
        }
    }

    for (auto it = torrents_.begin(); it != torrents_.end();) {
        Torrent& t = *it->second;
        if (t.removing) {
            const bool late = MsSince(t.removeAt) >= kRemoveWaitMs;
            if (t.deleted || late) {
                const bool gone = cache::DeleteTree(t.dir);
                if (gone || late) {  // a folder that will not go now is swept at the next start
                    Event e;
                    e.kind = Event::Kind::Removed;
                    e.ih = t.ih;
                    out->push_back(std::move(e));
                    it = torrents_.erase(it);
                    continue;
                }
            }
            ++it;
            continue;
        }
        if (t.metadata && !t.filesSent) {
            t.filesSent = true;
            Event e;
            e.kind = Event::Kind::Files;
            e.ih = t.ih;
            e.name = t.name;
            for (size_t i = 0; i < t.files.size() && e.files.size() < melange::media::proto::kMaxTorrentFiles; ++i) {
                const FileMeta& f = t.files[i];
                if (static_cast<int>(i) >= static_cast<int>(melange::media::proto::kMaxTorrentFiles)) break;
                if (f.path.empty() || f.pad) continue;
                FileInfo fi;
                fi.index = static_cast<int>(i);
                fi.path = f.path;
                fi.size = static_cast<uint64_t>(f.span.size);
                fi.video = f.playable;
                e.files.push_back(std::move(fi));
            }
            out->push_back(std::move(e));
        }
        if (MsSince(t.lastStatus) >= kStatusMs) {
            t.lastStatus = Clock::now();
            Event e;
            e.kind = Event::Kind::Status;
            e.ih = t.ih;
            try {
                const lt::torrent_status st = t.h.status();
                e.status.down = st.download_payload_rate;
                e.status.up = st.upload_payload_rate;
                e.status.peers = st.num_peers;
                e.status.metadata = st.has_metadata;
                if (t.selected >= 0 && t.metadata) {
                    std::vector<int64_t> fp;
                    t.h.file_progress(fp, lt::torrent_handle::piece_granularity);
                    const int64_t size = t.files[static_cast<size_t>(t.selected)].span.size;
                    if (static_cast<size_t>(t.selected) < fp.size() && size > 0) {
                        e.status.have = static_cast<uint64_t>(fp[static_cast<size_t>(t.selected)]);
                        e.status.progress = std::min(1.0, static_cast<double>(e.status.have) / static_cast<double>(size));
                    }
                }
                out->push_back(std::move(e));
            } catch (...) {
            }
        }
        ++it;
    }
}

void LtEngine::Shutdown() {
    std::unique_lock<std::mutex> lock(mu_);
    if (shutdown_) return;
    shutdown_ = true;
    if (!ses_) return;
    const Clock::time_point start = Clock::now();
    for (auto& [ih, t] : torrents_) {
        t->CancelStreams();
        if (!t->removing) {
            t->removing = true;
            try {
                if (t->h.is_valid()) ses_->remove_torrent(t->h, lt::session::delete_files);
                else t->deleted = true;
            } catch (...) {
                t->deleted = true;
            }
        }
    }
    auto pending = [&] {
        for (auto& [ih, t] : torrents_)
            if (!t->deleted) return true;
        return false;
    };
    while (pending() && MsSince(start) < kShutdownDeleteMs) {
        ses_->wait_for_alert(std::chrono::milliseconds(100));
        std::vector<lt::alert*> alerts;
        ses_->pop_alerts(&alerts);
        for (lt::alert* a : alerts) {
            if (auto* d = lt::alert_cast<lt::torrent_deleted_alert>(a)) {
                if (Torrent* t = FindByHashes(d->info_hashes)) t->deleted = true;
            } else if (auto* df = lt::alert_cast<lt::torrent_delete_failed_alert>(a)) {
                if (Torrent* t = FindByHashes(df->info_hashes)) t->deleted = true;
            }
        }
    }
    for (auto& [ih, t] : torrents_) cache::DeleteTree(t->dir);
    torrents_.clear();
    // The session's teardown (tracker "stopped" announces, the DHT) runs on its own thread; this waits at most
    // kShutdownAbortMs and then leaves it to the process exit, so Shutdown stays within its 5 s.
    auto ses = std::shared_ptr<lt::session>(std::move(ses_));
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> f = done->get_future();
    std::thread([ses, done]() mutable {
        try {
            lt::session_proxy proxy = ses->abort();
            ses.reset();
        } catch (...) {
        }
        done->set_value();
    }).detach();
    lock.unlock();
    f.wait_for(std::chrono::milliseconds(kShutdownAbortMs));
}

bool LtEngine::TestConnectPeer(const std::string& ih, const std::string& ip, int port) {
    std::lock_guard<std::mutex> lock(mu_);
    Torrent* t = Find(ih);
    if (!t) return false;
    lt::error_code ec;
    const auto addr = lt::make_address(ip, ec);
    if (ec) return false;
    try {
        t->h.connect_peer(lt::tcp::endpoint(addr, static_cast<uint16_t>(port)));
    } catch (...) {
        return false;
    }
    return true;
}
int LtEngine::TestCount(const std::string& ih, bool trackers) {
    std::lock_guard<std::mutex> lock(mu_);
    Torrent* t = Find(ih);
    if (!t) return -1;
    try {
        if (trackers) return static_cast<int>(t->h.trackers().size());
        return static_cast<int>(t->h.url_seeds().size());
    } catch (...) {
        return -1;
    }
}
}  // namespace

std::unique_ptr<Engine> CreateEngine(const Settings& s, std::string* err) {
    if (s.cacheDir.empty()) {
        if (err) *err = "no cache folder";
        return nullptr;
    }
    // Torrents of an earlier run of this instance (a crash, or a delete that failed) go before anything new.
    cache::SweepTorrents(s.cacheDir);
    return std::make_unique<LtEngine>(s);
}

namespace testing {
bool ConnectPeer(Engine* e, const std::string& ih, const std::string& ip, int port) {
    auto* lt = dynamic_cast<LtEngine*>(e);
    return lt && lt->TestConnectPeer(ih, ip, port);
}
void LoopbackOnly(bool on) { g_loopbackOnly = on; }
bool TrackerAllowed(const std::string& tracker) { return holo::bt::TrackerAllowed(tracker); }
int TrackerCount(Engine* e, const std::string& ih) {
    auto* lt = dynamic_cast<LtEngine*>(e);
    return lt ? lt->TestCount(ih, true) : -1;
}
int WebSeedCount(Engine* e, const std::string& ih) {
    auto* lt = dynamic_cast<LtEngine*>(e);
    return lt ? lt->TestCount(ih, false) : -1;
}
}  // namespace testing
}  // namespace holo::bt
