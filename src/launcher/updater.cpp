#include "launcher/updater.h"

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <miniz.h>

#include <cstdio>
#include <cstdlib>

#include "core/log.h"
#include "launcher/setup/dll_id.h"
#include "launcher/util.h"
#include "oasis/rpc/ini_edit.h"
#include "store/fetch.h"
#include "store/install.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::launcher::updater {
namespace {
constexpr size_t kMaxEntries = 2000;
constexpr uint64_t kMaxEntryBytes = 64ull << 20, kMaxTotalBytes = 512ull << 20;
// What the old exe's folder gets refreshed with, when it already has the file (an extracted release zip).
const wchar_t* const kOriginFiles[] = {L"melange.asi", L"dinput8.dll", L"Melange.ini", L"tools\\xomtool.exe", L"INSTALL.txt", L"LICENSE",
                                       L"THIRD_PARTY.md"};

bool Fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

std::string Hex(const BYTE* p, DWORD n) {
    static const char* const kDigits = "0123456789abcdef";
    std::string s;
    for (DWORD i = 0; i < n; ++i) {
        s += kDigits[p[i] >> 4];
        s += kDigits[p[i] & 15];
    }
    return s;
}

std::string SubjectOf(PCCERT_CONTEXT c) {
    const DWORD n = CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(n, L'\0');
    CertNameToStrW(X509_ASN_ENCODING, &c->pCertInfo->Subject, CERT_X500_NAME_STR, w.data(), n);
    w.resize(wcslen(w.c_str()));
    return Narrow(w);
}

std::string IssuerOrgOf(PCCERT_CONTEXT c) {
    const DWORD n = CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, CERT_NAME_ISSUER_FLAG, const_cast<char*>(szOID_ORGANIZATION_NAME), nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(n, L'\0');
    CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, CERT_NAME_ISSUER_FLAG, const_cast<char*>(szOID_ORGANIZATION_NAME), w.data(), n);
    w.resize(wcslen(w.c_str()));
    return Narrow(w);
}

std::string Str(const json::Value& o, const char* k) {
    const json::Value* v = o.Get(k);
    return v && v->IsString() ? v->string : std::string();
}

bool AccessDenied(unsigned long e) { return e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD || e == ERROR_WRITE_PROTECT; }

std::wstring Quote(const std::wstring& s) {
    std::wstring q = L"\"" + s;
    if (!s.empty() && s.back() == L'\\') q += L'\\';   // "C:\" would escape the closing quote
    return q + L"\"";
}

// Plain relative names: what scripts\zip.ps1 writes, nothing that could leave the folder.
bool SafeEntryName(const std::string& n) {
    if (n.empty() || n.size() > 240 || n[0] == '/') return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':') return false;
    std::string body = n;
    if (body.back() == '/') body.pop_back();
    size_t p = 0;
    int segs = 0;
    while (p <= body.size()) {
        size_t q = body.find('/', p);
        if (q == std::string::npos) q = body.size();
        const std::string s = body.substr(p, q - p);
        if (s.empty() || s == "." || s == ".." || s.back() == '.' || s.back() == ' ' || ++segs > 8) return false;
        p = q + 1;
    }
    return true;
}

struct FileSink : store::fetch::Sink {
    HANDLE f = INVALID_HANDLE_VALUE;
    hashutil::Sha256 sha;
    uint64_t n = 0;
    bool Write(const void* p, size_t len) override {
        DWORD w = 0;
        if (!WriteFile(f, p, static_cast<DWORD>(len), &w, nullptr) || w != len) return false;
        n += len;
        return sha.Update(p, len);
    }
};

size_t ZipWrite(void* op, mz_uint64, const void* buf, size_t n) {
    auto* f = static_cast<HANDLE*>(op);
    DWORD w = 0;
    return WriteFile(*f, buf, static_cast<DWORD>(n), &w, nullptr) && w == n ? n : 0;
}

// updates\<version>\ once Download finished it: ready.json for this version and the extracted Melange.exe.
bool ReadReady(const std::wstring& dir, const std::string& version, Staged* out) {
    json::Value v;
    json::Error e;
    if (!json::ParseFile(dir + L"\\ready.json", &v, &e) || !v.IsObject() || Str(v, "release") != version) return false;
    if (!FileExists(dir + L"\\payload\\Melange.exe")) return false;
    out->version = version;
    out->htmlUrl = Str(v, "htmlUrl");
    if (out->htmlUrl.rfind("https://", 0) != 0) out->htmlUrl.clear();
    out->sha256 = Str(v, "sha256");
    out->dir = dir;
    out->payload = dir + L"\\payload";
    return true;
}

// Copy beside the target, then one rename over it.
unsigned long ReplaceFile(const std::wstring& from, const std::wstring& to) {
    const std::wstring tmp = to + L".new-" + Widen(RandomHex(3));
    if (!CopyFileW(from.c_str(), tmp.c_str(), FALSE)) return GetLastError();
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long e = GetLastError();
        DeleteFileW(tmp.c_str());
        return e;
    }
    return 0;
}
}  // namespace

// ---------------------------------------------------------------- Authenticode
Signer ReadSigner(const std::wstring& file) {
    Signer s;
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    // No revocation lookups: they would fail offline when the update is applied, and the short-lived certificates are
    // timestamped. The chain must still reach a trusted root (Windows may fetch a missing intermediate) and the file
    // must be unmodified.
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG st = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    s.present = st != TRUST_E_NOSIGNATURE && st != TRUST_E_SUBJECT_FORM_UNKNOWN && st != TRUST_E_PROVIDER_UNKNOWN &&
                st != static_cast<LONG>(CRYPT_E_FILE_ERROR);
    s.valid = st == ERROR_SUCCESS;
    if (!s.valid) {
        char b[64];
        snprintf(b, sizeof b, "0x%08lx", static_cast<unsigned long>(st));
        s.error = st == TRUST_E_NOSIGNATURE ? "not signed" : st == TRUST_E_BAD_DIGEST ? "the file was changed after signing"
                  : st == static_cast<LONG>(CERT_E_UNTRUSTEDROOT) ? "the certificate is not trusted"
                  : st == static_cast<LONG>(CERT_E_EXPIRED) ? "the certificate expired" : std::string("signature check failed (") + b + ")";
    }
    if (wd.hWVTStateData) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        if (pc && pc->pCert) {
            s.subject = SubjectOf(pc->pCert);
            s.issuerOrg = IssuerOrgOf(pc->pCert);
            BYTE h[20];
            DWORD n = sizeof h;
            if (CertGetCertificateContextProperty(pc->pCert, CERT_SHA1_HASH_PROP_ID, h, &n)) s.thumbprint = Hex(h, n);
        }
        wd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    }
    return s;
}

bool SameSigner(const Signer& a, const Signer& b) { return !a.subject.empty() && a.subject == b.subject && a.issuerOrg == b.issuerOrg; }

std::string SignerRefusal(const Signer& running, const Signer& candidate, const std::string& what) {
    if (running.subject.empty()) return {};
    if (!candidate.valid) return what + " is not validly signed (" + (candidate.error.empty() ? "unknown" : candidate.error) + ")";
    if (!SameSigner(running, candidate))
        return what + " is signed by \"" + candidate.subject + "\", not by \"" + running.subject + "\" like this Melange.exe";
    return {};
}

// ---------------------------------------------------------------- download and stage
std::wstring Root() { return AppDataDir() + L"\\updates"; }

bool FetchLatest(const Source& src, update::Release* out, std::string* err) {
    store::fetch::StringSink sink;
    store::fetch::Options o;
    o.cap = update::kMaxApiBytes;
    o.totalMs = 30000;
    o.stallMs = 15000;
    o.cancel = src.cancel;
    o.registerActive = false;
    if (!src.userAgent.empty()) o.userAgent = src.userAgent;
    std::string why;
    if (!store::fetch::Get(src.latestUrl, sink, o, &why)) return Fail(err, "could not reach GitHub: " + why);
    return update::ParseRelease(sink.data, out, err);
}

bool ExtractZip(const std::wstring& zipPath, const std::wstring& dir, std::string* err) {
    FILE* f = _wfopen(zipPath.c_str(), L"rb");
    if (!f) return Fail(err, "cannot open the downloaded zip");
    mz_zip_archive z{};
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (size <= 0 || !mz_zip_reader_init_cfile(&z, f, static_cast<mz_uint64>(size), 0)) {
        fclose(f);
        return Fail(err, "the download is not a valid zip");
    }
    bool ok = true;
    std::string why;
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    uint64_t total = 0;
    if (n == 0 || n > kMaxEntries) ok = (why = "the zip has no entries or too many", false);
    if (ok && !MakeDirs(dir)) ok = (why = "cannot create " + Narrow(dir), false);
    for (mz_uint i = 0; ok && i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&z, i, &st)) {
            why = "unreadable zip entry";
            ok = false;
            break;
        }
        const std::string name = st.m_filename;
        if (!SafeEntryName(name)) {
            why = name.substr(0, 120) + ": not a plain relative name";
            ok = false;
            break;
        }
        if (st.m_is_encrypted || (st.m_method != 0 && st.m_method != MZ_DEFLATED) || !st.m_is_supported) {
            why = name + ": unsupported entry";
            ok = false;
            break;
        }
        total += st.m_uncomp_size;
        if (st.m_uncomp_size > kMaxEntryBytes || total > kMaxTotalBytes) {
            why = name + ": too large";
            ok = false;
            break;
        }
        std::wstring rel = Widen(name);
        for (auto& c : rel)
            if (c == L'/') c = L'\\';
        if (st.m_is_directory) {
            if (rel.back() == L'\\') rel.pop_back();
            if (!MakeDirs(dir + L"\\" + rel)) ok = (why = name + ": cannot create the folder", false);
            continue;
        }
        const std::wstring full = dir + L"\\" + rel;
        MakeDirs(Parent(full));
        HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            why = name + ": cannot create the file";
            ok = false;
            break;
        }
        const bool wrote = mz_zip_reader_extract_to_callback(&z, i, &ZipWrite, &h, 0) != 0;
        LARGE_INTEGER got{};
        GetFileSizeEx(h, &got);
        CloseHandle(h);
        if (!wrote || static_cast<uint64_t>(got.QuadPart) != st.m_uncomp_size) {
            why = name + ": corrupt entry";
            ok = false;
        }
    }
    mz_zip_reader_end(&z);
    fclose(f);
    if (!ok) return Fail(err, why);
    return true;
}

bool VerifyPayload(const std::wstring& payload, const std::string& version, const Signer& running, std::string* err) {
    for (const wchar_t* f : {L"Melange.exe", L"melange.asi", L"Melange.ini", L"dinput8.dll"})
        if (!FileExists(payload + L"\\" + f)) return Fail(err, "the release zip has no " + Narrow(f));
    for (const wchar_t* f : {L"Melange.exe", L"melange.asi"}) {
        const std::string v = setup::FileProductVersion(payload + L"\\" + f);
        if (v != version) return Fail(err, Narrow(f) + " in the release zip is version " + (v.empty() ? "unknown" : v) + ", not " + version);
    }
    if (running.subject.empty()) {
        LOG_INFO("[update] this Melange.exe is unsigned (a developer build): not checking the release's signatures");
        return true;
    }
    for (const wchar_t* f : {L"Melange.exe", L"melange.asi"}) {
        const Signer s = ReadSigner(payload + L"\\" + f);
        const std::string why = SignerRefusal(running, s, Narrow(f));
        if (!why.empty()) return Fail(err, why);
        LOG_INFO("[update] %ls signed by %s (%s)", f, s.subject.c_str(), s.thumbprint.c_str());
    }
    return true;
}

bool Download(const Source& src, const update::Release& rel, const std::wstring& root, const Signer& running,
              const std::function<void(uint64_t, uint64_t)>& progress, Staged* out, std::string* err) {
    const std::string v = rel.version;
    if (!update::PlainVersion(v)) return Fail(err, "bad release version");
    const update::Asset* ma = rel.Find(rel.ManifestName());
    if (!ma) return Fail(err, "the release has no " + rel.ManifestName());
    if (!update::AllowedUrl(ma->url, src.downloadPrefix)) return Fail(err, "the release manifest's address is not a Melange release download");
    const std::wstring dir = root + L"\\" + Widen(v);
    if (DirExists(dir) && !FileExists(dir + L"\\ready.json")) store::install::DeleteTree(dir);
    if (DirExists(dir)) {
        Staged s;
        if (ReadReady(dir, v, &s) && VerifyPayload(s.payload, v, running, err)) {
            *out = s;
            return true;
        }
        store::install::DeleteTree(dir);
    }
    if (!MakeDirs(dir)) return Fail(err, "cannot create " + Narrow(dir));
    auto bail = [&](const std::string& why) {
        store::install::DeleteTree(dir);
        return Fail(err, why);
    };

    store::fetch::Options o;
    o.cancel = src.cancel;
    o.registerActive = false;
    if (!src.userAgent.empty()) o.userAgent = src.userAgent;
    store::fetch::StringSink ms;
    o.cap = update::kMaxManifestBytes;
    o.totalMs = 30000;
    std::string why;
    if (!store::fetch::Get(ma->url, ms, o, &why)) return bail("could not download " + rel.ManifestName() + ": " + why);
    update::Manifest m;
    if (!update::ParseManifest(ms.data, v, &m, &why)) return bail(why);
    const update::Asset* za = rel.Find(m.zip);
    if (!za) return bail("the release has no " + m.zip);
    if (!update::AllowedUrl(za->url, src.downloadPrefix)) return bail("the release zip's address is not a Melange release download");
    if (za->size && za->size != m.size) return bail("the release lists " + m.zip + " at " + std::to_string(za->size) + " bytes, its manifest at " + std::to_string(m.size));
    if (WriteAtomic(dir + L"\\" + Widen(rel.ManifestName()), ms.data)) return bail("cannot write the release manifest");

    const std::wstring zip = dir + L"\\" + Widen(m.zip), part = zip + L".part";
    FileSink sink;
    sink.f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sink.f == INVALID_HANDLE_VALUE) return bail("cannot create the download file");
    o.cap = m.size;
    o.totalMs = 0;
    o.stallMs = 30000;
    o.progress = [&](uint64_t got, uint64_t) {
        if (progress) progress(got, m.size);
    };
    const bool got = store::fetch::Get(za->url, sink, o, &why);
    CloseHandle(sink.f);
    if (!got) return bail("could not download " + m.zip + ": " + why);
    const std::string sha = sink.sha.FinishHex();
    if (sink.n != m.size) return bail(m.zip + " is " + std::to_string(sink.n) + " bytes, expected " + std::to_string(m.size));
    if (sha != m.sha256) return bail(m.zip + " has SHA-256 " + sha + ", expected " + m.sha256);
    if (!MoveFileExW(part.c_str(), zip.c_str(), MOVEFILE_REPLACE_EXISTING)) return bail("cannot keep the download: " + Win32Message(GetLastError()));

    const std::wstring payload = dir + L"\\payload";
    if (!ExtractZip(zip, payload, &why)) return bail("the release zip is not usable: " + why);
    if (!VerifyPayload(payload, v, running, &why)) return bail(why);
    const std::string ready = jsonmini::Obj().Int("version", 1).Str("release", v).Str("htmlUrl", rel.htmlUrl).Str("sha256", m.sha256)
                                  .UInt("size", m.size).Str("at", NowIsoUtc()).End();
    if (WriteAtomic(dir + L"\\ready.json", ready)) return bail("cannot write ready.json");
    out->version = v;
    out->htmlUrl = rel.htmlUrl;
    out->sha256 = m.sha256;
    out->dir = dir;
    out->payload = payload;
    return true;
}

bool FindReady(const std::wstring& root, const std::string& current, Staged* out) {
    bool found = false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::string name = Narrow(fd.cFileName);
        if (!update::PlainVersion(name) || update::CompareVersions(name, current) <= 0) continue;
        if (found && update::CompareVersions(name, out->version) <= 0) continue;
        Staged s;
        if (!ReadReady(root + L"\\" + fd.cFileName, name, &s)) continue;
        *out = s;
        found = true;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

void Clean(const std::wstring& root, const std::string& keepVersion, const std::wstring& inUse) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> drop;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const std::wstring dir = root + L"\\" + fd.cFileName;
        if (!keepVersion.empty() && Narrow(fd.cFileName) == keepVersion) continue;
        if (!inUse.empty() && PathInside(inUse, dir)) continue;
        drop.push_back(dir);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const auto& d : drop) {
        if (!store::install::DeleteTree(d)) LOG_WARN("[update] could not delete %ls", d.c_str());
    }
}

// ---------------------------------------------------------------- apply
bool IsApplyCommand(const std::vector<std::wstring>& args) {
    for (const auto& a : args)
        if (a == L"--apply-update") return true;
    return false;
}

bool ParseApplyArgs(const std::vector<std::wstring>& args, ApplyArgs* out, std::string* err) {
    *out = ApplyArgs{};
    bool cmd = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == L"--apply-update") {
            cmd = true;
        } else if (a == L"--elevated") {
            out->elevated = true;
        } else if ((a == L"--from" || a == L"--game" || a == L"--result" || a == L"--pid") && hasValue) {
            const std::wstring v = args[++i];
            if (a == L"--pid") {
                if (v.empty() || v.size() > 10 || v.find_first_not_of(L"0123456789") != std::wstring::npos) return Fail(err, "--pid needs a process id");
                out->pid = std::wcstoul(v.c_str(), nullptr, 10);
                continue;
            }
            const bool absolute = (v.size() >= 3 && v[1] == L':' && (v[2] == L'\\' || v[2] == L'/')) || v.rfind(L"\\\\", 0) == 0;
            if (!absolute) return Fail(err, Narrow(a) + " needs an absolute path");
            (a == L"--from" ? out->from : a == L"--game" ? out->game : out->result) = FullPath(v);
        } else {
            return Fail(err, "unexpected argument " + Narrow(a).substr(0, 60));
        }
    }
    if (!cmd) return Fail(err, "not --apply-update");
    if (out->from.empty()) return Fail(err, "--from is required");
    if (!out->pid) return Fail(err, "--pid is required");
    return true;
}

std::wstring ApplyCommandLine(const std::wstring& exe, const ApplyArgs& a) {
    std::wstring s = (exe.empty() ? L"" : Quote(exe) + L" ") + L"--apply-update --from " + Quote(a.from) + L" --pid " + std::to_wstring(a.pid);
    if (!a.game.empty()) s += L" --game " + Quote(a.game);
    if (!a.result.empty()) s += L" --result " + Quote(a.result);
    if (a.elevated) s += L" --elevated";
    return s;
}

ApplyOutcome ApplyStaged(const setup::Context& ctx, const std::wstring& originDir) {
    ApplyOutcome r;
    // 1. The game folder, through the setup engine: staged, backed up, committed by rename, rolled back on failure.
    if (!ctx.gameDir.empty() && DirExists(ctx.gameDir)) {
        const setup::Status st = setup::Inspect(ctx);
        if (st.melangeState == "missing") {
            r.warnings.push_back("Melange isn't installed in " + Narrow(ctx.gameDir) + "; only Melange.exe was updated.");
        } else {
            setup::PlanRequest rq;
            rq.action = "install";
            const setup::Outcome o = setup::Apply(ctx, rq, "");
            if (!o.ok) {
                r.needElevation = o.code == -32010;
                r.message = o.message;
                return r;
            }
            r.gameUpdated = true;
            r.backupId = o.backupId;
            // An install switches a switched-off Melange back on: keep the user's choice.
            if (st.melangeState == "disabled") {
                const setup::Outcome off = setup::SetMelangeEnabled(ctx, false);
                if (!off.ok) r.warnings.push_back("Melange was updated but could not be switched off again: " + off.message);
            }
        }
    }
    // 2. The folder the old Melange.exe ran from, when that is somewhere else -- or the game folder itself when the
    // setup engine left it alone (Melange isn't installed there, but this Melange.exe is): then Melange.exe only.
    const bool originIsGame = PathKey(originDir) == PathKey(ctx.gameDir);
    if (!originDir.empty() && FileExists(originDir + L"\\Melange.exe") && (!originIsGame || !r.gameUpdated) &&
        PathKey(originDir) != PathKey(ctx.payloadDir)) {
        const std::wstring exe = originDir + L"\\Melange.exe", old = exe + L".old";
        if (hashutil::Sha256HexFile(exe) != hashutil::Sha256HexFile(ctx.payloadDir + L"\\Melange.exe")) {
            unsigned long e = MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : GetLastError();
            if (!e && !CopyFileW((ctx.payloadDir + L"\\Melange.exe").c_str(), exe.c_str(), FALSE)) {
                e = GetLastError();
                MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
            if (e) {
                r.needElevation = AccessDenied(e);
                r.message = "Could not update " + Narrow(exe) + ": " + Win32Message(e);
                return r;
            }
        }
        for (const wchar_t* f : kOriginFiles) {
            if (originIsGame) break;   // the game folder's other files belong to the setup engine (Melange.ini is the user's)
            const std::wstring to = originDir + L"\\" + f, from = ctx.payloadDir + L"\\" + f;
            if (!FileExists(to) || !FileExists(from) || hashutil::Sha256HexFile(to) == hashutil::Sha256HexFile(from)) continue;
            if (const unsigned long e = ReplaceFile(from, to)) r.warnings.push_back("Could not update " + Narrow(to) + ": " + Win32Message(e));
        }
    }
    r.ok = true;
    return r;
}

void DeleteOldExe(const std::wstring& dir) {
    if (dir.empty()) return;
    const std::wstring old = dir + L"\\Melange.exe.old";
    if (FileExists(old) && !DeleteFileW(old.c_str())) LOG_WARN("[update] could not delete %ls (%lu)", old.c_str(), GetLastError());
}

std::wstring ResultPath() { return Root() + L"\\result.json"; }

std::string ResultJson(const Result& r) {
    jsonmini::Arr w;
    for (const auto& x : r.warnings) w.Str(x);
    return jsonmini::Obj().Bool("ok", r.ok).Bool("gameUpdated", r.gameUpdated).Str("version", r.version).Str("message", r.message).Str("at", r.at).Raw("warnings", w.End()).End();
}

bool WriteResult(const std::wstring& path, const Result& r) {
    MakeDirs(Parent(path));
    return WriteAtomic(path, ResultJson(r)) == 0;
}

bool TakeResult(const std::wstring& path, Result* out) {
    *out = Result{};
    json::Value v;
    json::Error e;
    const bool parsed = json::ParseFile(path, &v, &e, 64u << 10) && v.IsObject();
    DeleteFileW(path.c_str());
    if (!parsed) return false;
    out->present = true;
    if (const json::Value* ok = v.Get("ok"); ok && ok->IsBool()) out->ok = ok->boolean;
    if (const json::Value* g = v.Get("gameUpdated"); g && g->IsBool()) out->gameUpdated = g->boolean;
    out->version = Str(v, "version");
    out->message = Str(v, "message");
    out->at = Str(v, "at");
    if (const json::Value* w = v.Get("warnings"); w && w->IsArray())
        for (const auto& x : w->items)
            if (x.IsString()) out->warnings.push_back(x.string);
    return true;
}

// ---------------------------------------------------------------- the in-game check
bool SyncInGameCheck(const std::wstring& gameDir, bool on, bool* changed, std::string* err) {
    *changed = false;
    const std::wstring path = gameDir.empty() ? std::wstring() : gameDir + L"\\Melange.ini";
    std::string bytes;
    if (path.empty() || !FileExists(path) || !ReadAll(path, &bytes, 4u << 20)) {
        LOG_INFO("[update] no Melange.ini%s%ls: the in-game check setting is not written", path.empty() ? "" : " in ", gameDir.c_str());
        return true;
    }
    oasis::ini::Encoding enc{};
    const std::string text = oasis::ini::Decode(bytes, &enc);
    const auto entries = oasis::ini::Parse(text);
    const oasis::ini::Entry* e = oasis::ini::Find(entries, "Update", "CheckInGame");
    const bool now = !e || e->value.empty() || strtol(e->value.c_str(), nullptr, 0) != 0;   // config::GetBool's reading
    if (now == on) return true;
    std::string out;
    if (!oasis::ini::Encode(oasis::ini::Set(text, "Update", "CheckInGame", on ? "1" : "0"), enc, &out))
        return Fail(err, "Melange.ini could not be written in its encoding");
    if (const unsigned long w = WriteAtomic(path, out)) return Fail(err, "could not write Melange.ini: " + Win32Message(w));
    LOG_INFO("[update] Melange.ini: [Update] CheckInGame=%d", on ? 1 : 0);
    *changed = true;
    return true;
}
}  // namespace melange::launcher::updater
