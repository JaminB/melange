#include "store/install.h"

#include <windows.h>

#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "store/index.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::store::install {
namespace {
constexpr uint64_t kMinFreeBytes = 512ull << 20;

std::wstring W(std::string_view s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) w.push_back(static_cast<unsigned char>(c));
    return w;
}

std::string Hex8() { return hashutil::RandomSalt().substr(0, 8); }

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool MakeDir(const std::wstring& p) {
    if (CreateDirectoryW(p.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && IsDir(p);
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

struct Sinker {
    HANDLE f = INVALID_HANDLE_VALUE;
    uint64_t limit = 0, written = 0;
    uint64_t* total = nullptr;
    uint64_t totalCap = 0;
    unsigned char head[4] = {};
    size_t headN = 0;
    bool magic = false, over = false, ioerr = false;
};

size_t WriteCb(void* op, mz_uint64, const void* buf, size_t n) {
    auto* s = static_cast<Sinker*>(op);
    const auto* b = static_cast<const unsigned char*>(buf);
    if (s->headN < 4) {
        const size_t k = (std::min)(4 - s->headN, n);
        memcpy(s->head + s->headN, b, k);
        s->headN += k;
        if (zipcheck::ExecutableMagic(s->head, s->headN)) {
            s->magic = true;
            return 0;
        }
    }
    if (s->written + n > s->limit || *s->total + n > s->totalCap) {
        s->over = true;
        return 0;
    }
    DWORD w = 0;
    if (!WriteFile(s->f, buf, static_cast<DWORD>(n), &w, nullptr) || w != n) {
        s->ioerr = true;
        return 0;
    }
    s->written += n;
    *s->total += n;
    return n;
}

struct Zip {
    mz_zip_archive z{};
    FILE* f = nullptr;
    bool open = false;
    ~Zip() {
        if (open) mz_zip_reader_end(&z);
        if (f) fclose(f);
    }
    bool Open(const std::wstring& path, std::string* err) {
        f = _wfopen(path.c_str(), L"rb");
        if (!f) {
            *err = "cannot open the downloaded file";
            return false;
        }
        _fseeki64(f, 0, SEEK_END);
        const long long size = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        if (size <= 0 || !mz_zip_reader_init_cfile(&z, f, static_cast<mz_uint64>(size), 0)) {
            *err = "not a valid zip file";
            return false;
        }
        open = true;
        return true;
    }
};

bool ListEntries(Zip& zip, std::vector<zipcheck::Entry>* out, std::string* err) {
    const mz_uint n = mz_zip_reader_get_num_files(&zip.z);
    if (n > zipcheck::kMaxEntries) {
        *err = "more than 2000 entries";
        return false;
    }
    out->clear();
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip.z, i, &st)) {
            *err = "unreadable zip entry";
            return false;
        }
        zipcheck::Entry e;
        char name[1024] = {};
        mz_zip_reader_get_filename(&zip.z, i, name, sizeof(name));
        e.name = name;
        e.compSize = st.m_comp_size;
        e.size = st.m_uncomp_size;
        e.method = st.m_method;
        e.flags = st.m_bit_flag;
        e.madeBy = st.m_version_made_by;
        e.externalAttr = st.m_external_attr;
        if (st.m_is_encrypted) e.flags |= 1;
        out->push_back(std::move(e));
    }
    return true;
}

bool ExtractAll(Zip& zip, const std::vector<zipcheck::Entry>& entries, const std::wstring& root, uint64_t totalCap,
                std::string* err, const std::atomic<bool>* cancel) {
    const std::wstring rootFinal = FinalPathOf(root);
    if (rootFinal.empty()) {
        *err = "cannot open the staging folder";
        return false;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (cancel && cancel->load()) {
            *err = "cancelled";
            return false;
        }
        const zipcheck::Entry& e = entries[i];
        std::wstring rel = W(e.name);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        const bool dir = rel.back() == L'\\';
        if (dir) rel.pop_back();
        size_t p = 0;
        while ((p = rel.find(L'\\', p)) != std::wstring::npos) {
            if (!MakeDir(root + L"\\" + rel.substr(0, p))) {
                *err = e.name + ": cannot create its folder";
                return false;
            }
            ++p;
        }
        const std::wstring full = root + L"\\" + rel;
        if (dir) {
            if (!MakeDir(full)) {
                *err = e.name + ": cannot create the folder";
                return false;
            }
            continue;
        }
        Sinker s;
        s.f = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s.f == INVALID_HANDLE_VALUE) {
            *err = e.name + ": cannot create the file";
            return false;
        }
        const std::wstring fin = FinalPath(s.f);
        if (fin.size() <= rootFinal.size() || fin.compare(0, rootFinal.size(), rootFinal) != 0 || fin[rootFinal.size()] != L'\\') {
            CloseHandle(s.f);
            *err = e.name + ": lands outside the staging folder";
            return false;
        }
        s.limit = e.size;
        s.total = &total;
        s.totalCap = totalCap;
        const bool ok = mz_zip_reader_extract_to_callback(&zip.z, static_cast<mz_uint>(i), &WriteCb, &s, 0) != 0;
        CloseHandle(s.f);
        if (!s.magic && s.headN < 4 && zipcheck::ExecutableMagic(s.head, s.headN)) s.magic = true;
        if (s.magic) {
            *err = e.name + ": executable content";
            return false;
        }
        if (s.over) {
            *err = e.name + ": inflates past its declared size";
            return false;
        }
        if (s.ioerr) {
            *err = e.name + ": write failed (disk full?)";
            return false;
        }
        if (!ok || s.written != e.size) {
            *err = e.name + ": corrupt entry";
            return false;
        }
    }
    return true;
}

std::string ReadAll(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (4u << 20)) out.append(buf, n);
    fclose(f);
    return out;
}
}  // namespace

Paths MakePaths(const std::wstring& modsDir) { return Paths{modsDir, modsDir + L"\\.store"}; }

bool EnsureDirs(const Paths& p) {
    bool ok = MakeDir(p.mods) && MakeDir(p.root) && MakeDir(p.Dl()) && MakeDir(p.Stage()) && MakeDir(p.Old()) && MakeDir(p.Cache());
    if (ok) SetFileAttributesW(p.root.c_str(), FILE_ATTRIBUTE_HIDDEN);
    return ok;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string EffectiveKind(const spice::Manifest& m) {
    const bool content = m.content || !m.entrySim.empty() || !m.messages.empty() || !m.weapons.empty() || !m.levels.empty() || m.unsafe;
    return content ? "content" : "client-only";
}

bool VerifyFile(const std::wstring& path, uint64_t size, const std::string& sha256, std::string* err) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    uint64_t got = 0;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        got = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    const std::string actual = got == size ? hashutil::Sha256HexFile(path) : std::string();
    if (got == size && actual == sha256) return true;
    *err = "Downloaded file does not match the store's record (";
    if (got != size)
        *err += "size " + std::to_string(got) + ", expected " + std::to_string(size);
    else
        *err += "sha256 " + actual + ", expected " + sha256;
    *err += "). Nothing was installed.";
    return false;
}

bool FetchVerified(const std::string& url, const std::wstring& part, const Expect& e, uint64_t cap,
                   const fetch::Options& base, std::string* err) {
    struct FileSink : fetch::Sink {
        HANDLE f = INVALID_HANDLE_VALUE;
        hashutil::Sha256 sha;
        uint64_t n = 0;
        bool Write(const void* p, size_t len) override {
            DWORD w = 0;
            if (!WriteFile(f, p, static_cast<DWORD>(len), &w, nullptr) || w != len) return false;
            n += len;
            return sha.Update(p, len);
        }
    } sink;
    sink.f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sink.f == INVALID_HANDLE_VALUE) {
        *err = "cannot create the download file";
        return false;
    }
    fetch::Options o = base;
    o.cap = cap;
    bool ok = fetch::Get(url, sink, o, err);
    CloseHandle(sink.f);
    if (ok) {
        const std::string actual = sink.sha.FinishHex();
        if (sink.n != e.size || actual != e.sha256) {
            *err = "Downloaded file does not match the store's record (";
            if (sink.n != e.size)
                *err += "size " + std::to_string(sink.n) + ", expected " + std::to_string(e.size);
            else
                *err += "sha256 " + actual + ", expected " + e.sha256;
            *err += "). Nothing was installed.";
            ok = false;
        }
    }
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}

bool Inspect(const std::wstring& zipPath, const std::string& id, uint64_t maxTotal, std::vector<zipcheck::Entry>* out,
             std::string* err) {
    Zip zip;
    if (!zip.Open(zipPath, err) || !ListEntries(zip, out, err)) return false;
    return zipcheck::Check(*out, id, maxTotal, err);
}

bool Stage(const Paths& p, const std::wstring& zipPath, const Expect& e, const std::string& melangeVersion, Staged* out,
           std::string* err, const std::atomic<bool>* cancel) {
    const uint64_t cap = (std::min)(e.unpackedSize ? e.unpackedSize : kMaxUnpackedBytes, kMaxUnpackedBytes);
    Zip zip;
    std::vector<zipcheck::Entry> entries;
    if (!zip.Open(zipPath, err) || !ListEntries(zip, &entries, err) || !zipcheck::Check(entries, e.id, cap, err)) return false;
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(p.mods.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < cap + kMinFreeBytes) {
        *err = "not enough free disk space (512 MiB must stay free)";
        return false;
    }
    if (!EnsureDirs(p)) {
        *err = "cannot create Mods\\.store";
        return false;
    }
    Staged s;
    const std::string name = e.id + "-" + Hex8();
    s.rel = "stage\\" + name;
    s.root = p.Stage() + L"\\" + W(name);
    s.dir = s.root + L"\\" + W(e.id);
    if (!CreateDirectoryW(s.root.c_str(), nullptr) || !IsDir(s.root)) {
        *err = "cannot create a staging folder";
        return false;
    }
    bool ok = ExtractAll(zip, entries, s.root, cap, err, cancel);
    if (ok) {
        spice::Manifest m;
        std::vector<spice::Error> errs;
        if (!spice::Parse(s.dir, &m, &errs) || m.implicit) {
            *err = "the plugin's spice.json is invalid" + (errs.empty() ? std::string() : ": " + errs.front().text);
            ok = false;
        } else if (m.id != e.id || m.version != e.version) {
            *err = "the zip holds " + m.id + " " + m.version + ", the store lists " + e.id + " " + e.version;
            ok = false;
        } else if (EffectiveKind(m) != e.kind || m.unsafe != e.unsafe || m.filesystem != e.filesystem) {
            *err = "the plugin declares more than its store listing (kind or permissions differ)";
            ok = false;
        } else if (!spice::SemverSatisfies(melangeVersion, m.melangeRange)) {
            *err = "the plugin needs Melange " + m.melangeRange;
            ok = false;
        }
    }
    if (!ok) {
        DeleteTree(s.root);
        return false;
    }
    *out = std::move(s);
    return true;
}

unsigned long DefaultMove(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), 0) ? 0 : GetLastError();
}

Result PlaceNew(const Paths& p, const std::string& id, const Staged& s, std::string* err, const MoveFn& mv) {
    const std::wstring target = p.mods + L"\\" + W(id);
    if (Exists(target)) {
        *err = "Mods\\" + id + " already exists";
        return Result::Failed;
    }
    if (const unsigned long e = mv(s.dir, target)) {
        *err = "cannot move the plugin into Mods\\" + id + " (error " + std::to_string(e) + ")";
        DeleteTree(s.root);
        return Result::Failed;
    }
    DeleteTree(s.root);
    return Result::Done;
}

Result Replace(const Paths& p, const std::string& id, const Staged& s, std::string* err, const MoveFn& mv) {
    const std::wstring target = p.mods + L"\\" + W(id);
    if (!Exists(target)) return PlaceNew(p, id, s, err, mv);
    if (!MakeDir(p.Old())) {
        *err = "cannot create Mods\\.store\\old";
        return Result::Failed;
    }
    const std::wstring old = p.Old() + L"\\" + W(id + "-" + Hex8());
    if (const unsigned long e = mv(target, old)) {
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION) {
            *err = "a file in Mods\\" + id + " is in use";
            return Result::Pending;
        }
        *err = "cannot move Mods\\" + id + " aside (error " + std::to_string(e) + ")";
        return Result::Failed;
    }
    const bool hadUser = IsDir(old + L"\\user");
    if (hadUser && mv(old + L"\\user", s.dir + L"\\user")) {
        mv(old, target);
        DeleteTree(s.root);
        *err = "cannot carry Mods\\" + id + "\\user over";
        return Result::Failed;
    }
    if (const unsigned long e = mv(s.dir, target)) {
        if (hadUser) mv(s.dir + L"\\user", old + L"\\user");
        const unsigned long back = mv(old, target);
        *err = "cannot move the new version into Mods\\" + id + " (error " + std::to_string(e) + ")";
        if (back) *err += "; the old copy is in Mods\\.store\\old";
        DeleteTree(s.root);
        return Result::Failed;
    }
    DeleteTree(old);
    DeleteTree(s.root);
    return Result::Done;
}

Result Remove(const Paths& p, const std::string& id, std::string* err, const MoveFn& mv) {
    const std::wstring target = p.mods + L"\\" + W(id);
    if (!Exists(target)) return Result::Done;
    if (!MakeDir(p.Old())) {
        *err = "cannot create Mods\\.store\\old";
        return Result::Failed;
    }
    const std::wstring old = p.Old() + L"\\" + W(id + "-" + Hex8());
    if (const unsigned long e = mv(target, old)) {
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION) {
            *err = "a file in Mods\\" + id + " is in use";
            return Result::Pending;
        }
        *err = "cannot remove Mods\\" + id + " (error " + std::to_string(e) + ")";
        return Result::Failed;
    }
    DeleteTree(old);
    return Result::Done;
}

bool DeleteTree(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(dir.c_str()) != 0;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(dir.c_str()) != 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            DeleteTree(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

std::string NowIso() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool LoadDb(const Paths& p, Db* db) {
    *db = Db{};
    json::Value v;
    json::Error e;
    if (!json::ParseFile(p.root + L"\\installed.json", &v, &e) || !v.IsObject()) return false;
    for (const auto& [k, m] : v.members) {
        if (k == "_serialSeen") {
            if (m.IsInteger()) db->serialSeen = static_cast<long long>(m.number);
            continue;
        }
        if (!m.IsObject() || !spice::ValidModId(k)) continue;
        Record r;
        if (const json::Value* x = m.Get("version"); x && x->IsString()) r.version = x->string;
        if (const json::Value* x = m.Get("sha256"); x && x->IsString()) r.sha256 = x->string;
        if (const json::Value* x = m.Get("installedAt"); x && x->IsString()) r.installedAt = x->string;
        if (const json::Value* x = m.Get("serial"); x && x->IsInteger()) r.serial = static_cast<long long>(x->number);
        db->mods[k] = r;
    }
    return true;
}

bool SaveDb(const Paths& p, const Db& db) {
    jsonmini::Obj o;
    o.Int("_serialSeen", db.serialSeen);
    for (const auto& [id, r] : db.mods)
        o.Raw(id, jsonmini::Obj().Str("version", r.version).Str("sha256", r.sha256).Str("installedAt", r.installedAt)
                      .Int("serial", r.serial).End());
    return MakeDir(p.root) && WriteFileAtomic(p.root + L"\\installed.json", o.End() + "\n");
}

bool LoadPending(const Paths& p, std::vector<Pending>* out) {
    out->clear();
    json::Value v;
    json::Error e;
    if (!json::ParseFile(p.root + L"\\pending.json", &v, &e) || !v.IsArray()) return false;
    for (const json::Value& m : v.items) {
        if (!m.IsObject()) continue;
        Pending op;
        auto str = [&](const char* k, std::string* s) {
            if (const json::Value* x = m.Get(k); x && x->IsString()) *s = x->string;
        };
        str("op", &op.op);
        str("id", &op.id);
        str("version", &op.version);
        str("sha256", &op.sha256);
        str("stage", &op.stage);
        if (const json::Value* x = m.Get("serial"); x && x->IsInteger()) op.serial = static_cast<long long>(x->number);
        if (const json::Value* x = m.Get("deleteData"); x && x->IsBool()) op.deleteData = x->boolean;
        if ((op.op != "update" && op.op != "remove") || !spice::ValidModId(op.id)) continue;
        out->push_back(std::move(op));
    }
    return true;
}

bool SavePending(const Paths& p, const std::vector<Pending>& ops) {
    const std::wstring path = p.root + L"\\pending.json";
    if (ops.empty()) {
        DeleteFileW(path.c_str());
        return true;
    }
    jsonmini::Arr a;
    for (const Pending& op : ops)
        a.Raw(jsonmini::Obj().Str("op", op.op).Str("id", op.id).Str("version", op.version).Str("sha256", op.sha256)
                  .Str("stage", op.stage).Int("serial", op.serial).Bool("deleteData", op.deleteData).End());
    return MakeDir(p.root) && WriteFileAtomic(path, a.End() + "\n");
}

std::vector<Applied> ApplyPending(const Paths& p, Db* db, const MoveFn& mv) {
    std::vector<Pending> ops, keep;
    std::vector<Applied> out;
    if (!LoadPending(p, &ops)) return out;
    for (const Pending& op : ops) {
        Applied a;
        a.op = op;
        std::string err;
        Result r = Result::Failed;
        if (op.op == "remove") {
            r = Remove(p, op.id, &err, mv);
            if (r == Result::Done) db->mods.erase(op.id);
        } else {
            Staged s;
            s.rel = op.stage;
            s.root = p.root + L"\\" + W(op.stage);
            s.dir = s.root + L"\\" + W(op.id);
            spice::Manifest m;
            std::vector<spice::Error> errs;
            const bool safeRel = op.stage.starts_with("stage\\") && op.stage.find("..") == std::string::npos &&
                                 op.stage.find('/') == std::string::npos && op.stage.find('\\', 6) == std::string::npos;
            if (!safeRel || !IsDir(s.dir) || !spice::Parse(s.dir, &m, &errs) || m.version != op.version || m.id != op.id) {
                err = "the staged update is missing or changed";
                if (safeRel) DeleteTree(s.root);
            } else {
                r = Replace(p, op.id, s, &err, mv);
                if (r == Result::Done) db->mods[op.id] = Record{op.version, op.sha256, NowIso(), op.serial};
            }
        }
        a.ok = r == Result::Done;
        a.message = a.ok ? op.op + " of " + op.id + (op.version.empty() ? "" : " " + op.version) + " applied" : err;
        if (r == Result::Pending) keep.push_back(op);
        out.push_back(std::move(a));
    }
    SavePending(p, keep);
    SaveDb(p, *db);
    return out;
}

void CleanLeftovers(const Paths& p) {
    std::vector<Pending> ops;
    LoadPending(p, &ops);
    std::set<std::wstring> keep;
    for (const Pending& op : ops)
        if (!op.stage.empty()) keep.insert(p.root + L"\\" + W(op.stage));
    for (const std::wstring& dir : {p.Dl(), p.Old(), p.Stage()}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring full = dir + L"\\" + fd.cFileName;
            if (!keep.count(full)) DeleteTree(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}
}  // namespace melange::store::install
