// The media pack's disk side (pack_internal.h): the streaming extractor, the install into place, the launch-time
// re-hash, the sweep and the uninstall. Not zipcheck: the pack is mostly executables and its files are far larger than a
// plugin's, so it has its own rules (pack_logic.h PlanExtract) and streams every entry to disk (Critique M1: the game
// is a 2 GB process at about 1.5 GB in a match, so no entry-sized buffer, and at most 4 MiB of heap in all).
#include <windows.h>

#include <miniz.h>

#include <malloc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "core/log.h"
#include "media/pack_internal.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::media::pack::core {
namespace {
HeapCounter g_heap;

void Count(int64_t delta) {
    const int64_t now = g_heap.current.fetch_add(delta) + delta;
    int64_t peak = g_heap.peak.load();
    while (now > peak && !g_heap.peak.compare_exchange_weak(peak, now)) {
    }
}

void* MzAlloc(void*, size_t items, size_t size) { return CountedAlloc(items * size); }
void MzFree(void*, void* p) { CountedFree(p); }
void* MzRealloc(void*, void* p, size_t items, size_t size) {
    const int64_t old = p ? static_cast<int64_t>(_msize(p)) : 0;
    void* q = realloc(p, items * size);
    if (q) Count(static_cast<int64_t>(_msize(q)) - old);
    return q;
}

std::wstring FinalPath(HANDLE h) {
    wchar_t buf[1024];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= 1024) return {};
    std::wstring s(buf, n);
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring FinalPathOf(const std::wstring& dir) {
    HANDLE h = CreateFileW(dir.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring s = FinalPath(h);
    CloseHandle(h);
    return s;
}

bool Inside(const std::wstring& fin, const std::wstring& rootFinal) {
    return !rootFinal.empty() && fin.size() > rootFinal.size() && fin.compare(0, rootFinal.size(), rootFinal) == 0 &&
           fin[rootFinal.size()] == L'\\';
}

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Native(const std::string& rel) {
    std::wstring w = Widen(rel);
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    return w;
}

std::string RandHex() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    char b[24];
    snprintf(b, sizeof b, "%08lx%04lx", static_cast<unsigned long>(q.LowPart ^ GetCurrentProcessId() * 2654435761u),
             static_cast<unsigned long>(GetTickCount64() & 0xffff));
    return b;
}

// Renames `dir` to "<dir>.del-<hex>.tmp" and deletes that. False when `dir` cannot be moved (a file in it is open).
bool MoveAsideAndDelete(const std::wstring& dir) {
    if (!IsDir(dir)) return true;
    const std::wstring aside = dir + L".del-" + Widen(RandHex()) + L".tmp";
    if (!MoveFileExW(dir.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
    if (!DeleteTree(aside)) LOG_WARN("[media] could not delete all of %s; the next start retries", Narrow(aside).c_str());
    return true;
}

uint64_t WriteTimeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

uint64_t SizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return UINT64_MAX;
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

struct Zip {
    mz_zip_archive z{};
    FILE* f = nullptr;
    bool open = false;
    ~Zip() {
        if (open) mz_zip_reader_end(&z);
        if (f) fclose(f);
    }
    bool Open(const std::wstring& path) {
        f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        setvbuf(f, nullptr, _IONBF, 0);  // miniz reads in 64 KB blocks; no second CRT buffer
        _fseeki64(f, 0, SEEK_END);
        const long long size = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        z.m_pAlloc = &MzAlloc;
        z.m_pFree = &MzFree;
        z.m_pRealloc = &MzRealloc;
        if (size <= 0 || !mz_zip_reader_init_cfile(&z, f, static_cast<mz_uint64>(size), 0)) return false;
        open = true;
        return true;
    }
};

// One entry's write side: a counted 1 MiB buffer (shared across entries), the running hash, the size limit.
struct Sinker {
    HANDLE f = INVALID_HANDLE_VALUE;
    char* buf = nullptr;
    size_t fill = 0;
    uint64_t limit = 0, written = 0;
    std::optional<hashutil::Sha256> sha;
    const Hooks* h = nullptr;
    uint64_t* done = nullptr;
    uint64_t total = 0;
    ULONGLONG lastProgress = 0;
    bool over = false, ioerr = false, cancelled = false;
    bool Flush() {
        DWORD w = 0;
        if (fill && (!WriteFile(f, buf, static_cast<DWORD>(fill), &w, nullptr) || w != fill)) {
            ioerr = true;
            return false;
        }
        fill = 0;
        return true;
    }
};

size_t WriteCb(void* op, mz_uint64, const void* data, size_t n) {
    auto* s = static_cast<Sinker*>(op);
    if (s->h->cancel && s->h->cancel->load()) {
        s->cancelled = true;
        return 0;
    }
    if (s->written + n > s->limit) {
        s->over = true;
        return 0;
    }
    if (!s->sha->Update(data, n)) {
        s->ioerr = true;
        return 0;
    }
    const char* p = static_cast<const char*>(data);
    size_t left = n;
    while (left) {
        const size_t k = (std::min)(left, kIoBufferBytes - s->fill);
        memcpy(s->buf + s->fill, p, k);
        s->fill += k;
        p += k;
        left -= k;
        if (s->fill == kIoBufferBytes && !s->Flush()) return 0;
    }
    s->written += n;
    *s->done += n;
    const ULONGLONG now = GetTickCount64();
    if (s->h->progress && now - s->lastProgress >= 100) {
        s->lastProgress = now;
        s->h->progress(*s->done, s->total);
    }
    return n;
}

bool Fail(std::string* error, std::string* detail, const char* e, std::string d) {
    *error = e;
    *detail = std::move(d);
    return false;
}
}  // namespace

HeapCounter& Heap() { return g_heap; }

void* CountedAlloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (p) Count(static_cast<int64_t>(_msize(p)));
    return p;
}

void CountedFree(void* p) {
    if (!p) return;
    Count(-static_cast<int64_t>(_msize(p)));
    free(p);
}

std::wstring Layout::PackDir(const std::string& ver) const { return root + L"\\" + Widen(ver); }

bool ExtractPack(const std::wstring& zipPath, const logic::Manifest& m, const std::wstring& dest, const Hooks& h,
                 std::string* error, std::string* detail) {
    Zip zip;
    if (!zip.Open(zipPath)) return Fail(error, detail, "zip", "not a readable zip");
    std::vector<logic::ZipEntry> entries;
    const mz_uint n = mz_zip_reader_get_num_files(&zip.z);
    if (n > logic::kMaxFiles * 4) return Fail(error, detail, "zip", "the zip has too many entries");
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip.z, i, &st)) return Fail(error, detail, "zip", "unreadable zip entry");
        // The full name, not the stat's (it truncates at 512 bytes, which could make two names look alike).
        char name[512] = {};
        const mz_uint len = mz_zip_reader_get_filename(&zip.z, i, nullptr, 0);
        if (len == 0 || len > sizeof name) return Fail(error, detail, "zip", "a zip entry name is too long");
        mz_zip_reader_get_filename(&zip.z, i, name, sizeof name);
        logic::ZipEntry e;
        e.name = name;
        e.size = st.m_uncomp_size;
        e.dir = st.m_is_directory != 0;
        e.encrypted = st.m_is_encrypted || !st.m_is_supported;
        e.method = st.m_method;
        entries.push_back(std::move(e));
    }
    std::vector<int> plan;
    std::string why;
    if (!logic::PlanExtract(entries, m, &plan, &why)) return Fail(error, detail, "zip", why);

    if (!CreateDirectoryW(dest.c_str(), nullptr)) return Fail(error, detail, "disk", "cannot create " + Narrow(dest));
    const std::wstring rootFinal = FinalPathOf(dest);
    if (rootFinal.empty()) return Fail(error, detail, "disk", "cannot open " + Narrow(dest));
    Sinker s;
    s.h = &h;
    s.buf = static_cast<char*>(CountedAlloc(kIoBufferBytes));
    uint64_t done = 0;
    s.done = &done;
    s.total = m.UnpackedBytes();
    bool ok = s.buf != nullptr;
    if (!ok) Fail(error, detail, "disk", "out of memory");
    for (size_t i = 0; ok && i < entries.size(); ++i) {
        if (plan[i] < 0) continue;
        const logic::File& file = m.files[static_cast<size_t>(plan[i])];
        const std::wstring rel = Native(file.path);
        for (size_t p = rel.find(L'\\'); p != std::wstring::npos; p = rel.find(L'\\', p + 1)) {
            const std::wstring sub = dest + L"\\" + rel.substr(0, p);
            if (!CreateDirectoryW(sub.c_str(), nullptr) && !IsDir(sub)) {
                ok = Fail(error, detail, "disk", "cannot create the folder of " + file.path);
                break;
            }
        }
        if (!ok) break;
        const std::wstring full = dest + L"\\" + rel;
        s.f = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s.f == INVALID_HANDLE_VALUE) {
            ok = Fail(error, detail, "disk", "cannot create " + file.path);
            break;
        }
        if (!Inside(FinalPath(s.f), rootFinal)) {
            CloseHandle(s.f);
            ok = Fail(error, detail, "zip", file.path + ": lands outside the pack folder");
            break;
        }
        s.fill = 0;
        s.written = 0;
        s.limit = file.size;
        s.over = s.ioerr = false;
        s.sha.emplace();
        const bool extracted = mz_zip_reader_extract_to_callback(&zip.z, static_cast<mz_uint>(i), &WriteCb, &s, 0) != 0;
        const bool flushed = s.Flush();
        CloseHandle(s.f);
        if (s.cancelled) {
            ok = Fail(error, detail, "cancelled", "cancelled");
        } else if (s.ioerr || !flushed) {
            ok = Fail(error, detail, "disk", file.path + ": write failed (disk full?)");
        } else if (s.over) {
            ok = Fail(error, detail, "hash", file.path + ": inflates past its size");
        } else if (!extracted || s.written != file.size) {
            ok = Fail(error, detail, "hash", file.path + ": corrupt entry");
        } else if (const std::string got = s.sha->FinishHex(); got != file.sha256) {
            ok = Fail(error, detail, "hash", file.path + ": sha256 " + got + ", expected " + file.sha256);
        }
    }
    CountedFree(s.buf);
    if (ok && h.progress) h.progress(done, s.total);
    if (!ok) DeleteTree(dest);
    return ok;
}

bool InstallPack(const Layout& l, const logic::Manifest& m, const std::string& url, const Hooks& h, std::string* error,
                 std::string* detail) {
    std::string why;
    if (!logic::CheckManifest(m, &why)) return Fail(error, detail, "manifest", why);
    if (!MakeDirs(l.root) || !MakeDirs(l.Dl())) return Fail(error, detail, "disk", "cannot create " + Narrow(l.root));
    const std::wstring part = l.Dl() + L"\\pack-" + Widen(m.version) + L"-" + Widen(m.zipSha256.substr(0, 8)) + L".zip.part";
    const uint64_t have = SizeOf(part) == UINT64_MAX ? 0 : SizeOf(part);
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(l.root.c_str(), &freeBytes, nullptr, nullptr) &&
        freeBytes.QuadPart + have < logic::NeedFreeBytes(m))
        return Fail(error, detail, "disk-space",
                    "needs " + std::to_string((logic::NeedFreeBytes(m) >> 20) + 1) + " MiB free on the Documents drive");
    if (h.phase) h.phase("downloading");
    if (!FetchResumable(url, part, m.zipSize, m.zipSha256, h, error, detail)) return false;
    if (h.phase) h.phase("installing");

    const std::wstring tmp = l.PackDir(m.version) + L".tmp";
    if (IsDir(tmp) && !DeleteTree(tmp)) return Fail(error, detail, "disk", "cannot clear " + Narrow(tmp));
    if (!ExtractPack(part, m, tmp, h, error, detail)) {
        // A zip that matched its hash but fails extraction will fail again: drop it.
        if (*error != "cancelled" && *error != "disk") DeleteFileW(part.c_str());
        return false;
    }
    std::string text;
    {
        FILE* f = _wfopen((tmp + L"\\pack.json").c_str(), L"rb");
        char b[4096];
        size_t k;
        while (f && (k = fread(b, 1, sizeof b, f)) > 0 && text.size() <= logic::kMaxManifestBytes) text.append(b, k);
        if (f) fclose(f);
    }
    logic::PackJson pj;
    if (!logic::ParsePackJson(text, &pj, &why) || !logic::PackJsonMatches(pj, m, &why)) {
        DeleteTree(tmp);
        DeleteFileW(part.c_str());
        return Fail(error, detail, "manifest", why);
    }
    const std::wstring final_ = l.PackDir(m.version);
    if (!MoveAsideAndDelete(final_)) {
        DeleteTree(tmp);
        return Fail(error, detail, "in-use", "the old pack folder is in use");
    }
    if (!MoveFileExW(tmp.c_str(), final_.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteTree(tmp);
        return Fail(error, detail, "disk", "cannot rename " + Narrow(tmp));
    }
    if (!RecordInstalled(l, m)) LOG_WARN("[media] could not write installed.json; the next launch re-verifies the pack");
    DeleteFileW(part.c_str());
    // Older pack versions: folders with a pack.json and a version name, other than this one.
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileExW((l.root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    if (fh != INVALID_HANDLE_VALUE) {
        std::vector<std::wstring> old;
        do {
            const std::string name = Narrow(fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                logic::ValidVersion(name) && name != m.version && !logic::IsTmpFolder(name) &&
                GetFileAttributesW((l.root + L"\\" + fd.cFileName + L"\\pack.json").c_str()) != INVALID_FILE_ATTRIBUTES)
                old.push_back(l.root + L"\\" + fd.cFileName);
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
        for (const std::wstring& d : old)
            if (MoveAsideAndDelete(d)) LOG_INFO("[media] removed the old media pack %s", Narrow(d).c_str());
    }
    return true;
}

bool PackPresent(const Layout& l, const logic::Manifest& m) {
    if (m.files.empty()) return false;
    const std::wstring dir = l.PackDir(m.version);
    for (const logic::File& f : m.files)
        if (SizeOf(dir + L"\\" + Native(f.path)) != f.size) return false;
    return true;
}

bool VerifyPackFiles(const Layout& l, const logic::Manifest& m, std::string* detail) {
    const std::wstring dir = l.PackDir(m.version);
    for (const logic::File& f : m.files) {
        HANDLE held = INVALID_HANDLE_VALUE;
        const bool ok = OpenHashed(dir + L"\\" + Native(f.path), dir, f.size, f.sha256, &held, detail);
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        if (!ok) return false;
    }
    return true;
}

uint64_t RecordedPackJsonTime(const Layout& l, const logic::Manifest& m) {
    json::Value v;
    json::Error e;
    if (!json::ParseFile(l.root + L"\\installed.json", &v, &e, logic::kMaxManifestBytes) || !v.IsObject()) return 0;
    const json::Value* ver = v.Get("version");
    const json::Value* t = v.Get("packJsonTime");
    if (!ver || !ver->IsString() || ver->string != m.version || !t || !t->IsString() || t->string.empty() || t->string.size() > 20)
        return 0;
    for (char c : t->string)
        if (c < '0' || c > '9') return 0;
    return std::strtoull(t->string.c_str(), nullptr, 10);
}

uint64_t PackJsonTime(const Layout& l, const logic::Manifest& m) { return WriteTimeOf(l.PackDir(m.version) + L"\\pack.json"); }

bool RecordInstalled(const Layout& l, const logic::Manifest& m) {
    jsonmini::Obj o;
    // A FILETIME does not fit a JSON number's 53 bits: kept as a decimal string.
    o.Str("version", m.version).Str("packJsonTime", std::to_string(PackJsonTime(l, m))).Int("recordedAt", UnixNow());
    return WriteFileAtomic(l.root + L"\\installed.json", o.End() + "\n");
}

void Sweep(const Layout& l) {
    auto each = [](const std::wstring& dir, bool dirs, auto&& fn) {
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                     dirs ? FindExSearchLimitToDirectories : FindExSearchNameMatch, nullptr, 0);
        if (fh == INVALID_HANDLE_VALUE) return;
        std::vector<WIN32_FIND_DATAW> all;
        do {
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) all.push_back(fd);
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
        for (const WIN32_FIND_DATAW& x : all) fn(x);
    };
    for (const std::wstring& root : {l.root, l.LaneRoot()})
        each(root, true, [&](const WIN32_FIND_DATAW& fd) {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && logic::IsTmpFolder(Narrow(fd.cFileName))) {
                const std::wstring p = root + L"\\" + fd.cFileName;
                if (DeleteTree(p)) LOG_INFO("[media] swept %s", Narrow(p).c_str());
            }
        });
    const int64_t now = UnixNow();
    each(l.Dl(), false, [&](const WIN32_FIND_DATAW& fd) {
        const std::wstring name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name.size() < 6 || name.substr(name.size() - 5) != L".part") return;
        const uint64_t t = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        const int64_t age = now - (static_cast<int64_t>(t / 10000000ull) - 11644473600ll);
        if (age > 7 * 24 * 3600) DeleteFileW((l.Dl() + L"\\" + name).c_str());
    });
}

bool Uninstall(const Layout& l, std::string* error) {
    // Move every pack folder and the lane aside first: that fails while a file in them is open (a running helper), and
    // then nothing is deleted.
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileExW((l.root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = Narrow(fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && logic::ValidVersion(name) && !logic::IsTmpFolder(name) &&
                GetFileAttributesW((l.root + L"\\" + fd.cFileName + L"\\pack.json").c_str()) != INVALID_FILE_ATTRIBUTES)
                dirs.push_back(l.root + L"\\" + fd.cFileName);
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    if (IsDir(l.LaneRoot())) dirs.push_back(l.LaneRoot());
    std::vector<std::pair<std::wstring, std::wstring>> moved;
    for (const std::wstring& d : dirs) {
        const std::wstring aside = d + L".del-" + Widen(RandHex()) + L".tmp";
        if (!MoveFileExW(d.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH)) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it) MoveFileExW(it->second.c_str(), it->first.c_str(), 0);
            *error = "in-use";
            return false;
        }
        moved.emplace_back(d, aside);
    }
    bool ok = true;
    for (const auto& [from, aside] : moved) ok = DeleteTree(aside) && ok;
    DeleteFileW((l.root + L"\\installed.json").c_str());
    DeleteTree(l.Dl());
    if (!ok) LOG_WARN("[media] some media pack files could not be deleted; the next start retries");
    return true;
}

bool DeleteTree(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(dir.c_str()) != 0;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(dir.c_str()) != 0;  // the link, never its target
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                ok = ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? RemoveDirectoryW(p.c_str()) != 0 : DeleteTree(p)) && ok;
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                ok = DeleteFileW(p.c_str()) && ok;
            }
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    return RemoveDirectoryW(dir.c_str()) && ok;
}

bool OpenHashed(const std::wstring& path, const std::wstring& inside, uint64_t size, const std::string& sha256, HANDLE* held,
                std::string* detail) {
    *held = INVALID_HANDLE_VALUE;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *detail = Narrow(path) + ": cannot open it (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto fail = [&](std::string d) {
        CloseHandle(f);
        *detail = Narrow(path) + ": " + d;
        return false;
    };
    BY_HANDLE_FILE_INFORMATION bi{};
    if (!GetFileInformationByHandle(f, &bi) || (bi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return fail("not a plain file");
    if (!inside.empty() && !Inside(FinalPath(f), FinalPathOf(inside))) return fail("lies outside its folder");
    const uint64_t got = (static_cast<uint64_t>(bi.nFileSizeHigh) << 32) | bi.nFileSizeLow;
    if (got != size) return fail("size " + std::to_string(got) + ", expected " + std::to_string(size));
    char* buf = static_cast<char*>(CountedAlloc(kIoBufferBytes));
    if (!buf) return fail("out of memory");
    hashutil::Sha256 sha;
    bool ok = true;
    for (;;) {
        DWORD rd = 0;
        if (!ReadFile(f, buf, static_cast<DWORD>(kIoBufferBytes), &rd, nullptr)) {
            ok = false;
            break;
        }
        if (!rd) break;
        if (!sha.Update(buf, rd)) {
            ok = false;
            break;
        }
    }
    CountedFree(buf);
    if (!ok) return fail("cannot read it");
    const std::string hex = sha.FinishHex();
    if (hex != sha256) return fail("sha256 " + hex + ", expected " + sha256);
    LARGE_INTEGER zero{};
    SetFilePointerEx(f, zero, nullptr, FILE_BEGIN);
    *held = f;
    return true;
}
}  // namespace melange::media::pack::core
