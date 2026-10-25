#include "sandbox.h"

#include <wincrypt.h>

#include <cwctype>
#include <thread>

#include "log.h"
#include "util.h"

namespace holo::sandbox {
uint32_t IntegrityRid(HANDLE token) {
    HANDLE t = token;
    if (!t && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    uint32_t rid = 0;
    DWORD n = 0;
    GetTokenInformation(t, TokenIntegrityLevel, nullptr, 0, &n);
    std::vector<uint8_t> buf(n ? n : 1);
    if (n && GetTokenInformation(t, TokenIntegrityLevel, buf.data(), n, &n)) {
        PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data())->Label.Sid;
        rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1u);
    }
    if (!token) CloseHandle(t);
    return rid;
}

const char* IntegrityName(uint32_t rid) {
    if (rid < SECURITY_MANDATORY_LOW_RID) return "untrusted";
    if (rid < SECURITY_MANDATORY_MEDIUM_RID) return "low";
    if (rid < SECURITY_MANDATORY_HIGH_RID) return "medium";
    return "high";
}

bool PrepareCache(const std::wstring& cache, std::string* why) {
    CreateDirectoryW(cache.c_str(), nullptr);
    const DWORD a = GetFileAttributesW(cache.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (why) *why = "cache folder missing or a reparse point";
        return false;
    }
    for (const wchar_t* sub : {L"\\mpvhome", L"\\home", L"\\home\\appdata", L"\\deno", L"\\tmp"}) CreateDirectoryW((cache + sub).c_str(), nullptr);
    return true;
}

void ScrubEnvironment(const std::wstring& cache) {
    // Variables that change what yt-dlp (Python), deno or mpv load, trust or log.
    static const wchar_t* const kPrefixes[] = {L"PYTHON", L"YTDLP", L"YT_DLP", L"MPV_", L"DENO_", L"XDG_", L"NODE_", L"LUA_"};
    static const wchar_t* const kNames[] = {L"HOME", L"SSLKEYLOGFILE", L"SSL_CERT_FILE", L"SSL_CERT_DIR", L"REQUESTS_CA_BUNDLE",
                                            L"CURL_CA_BUNDLE", L"WEBSOCKET_CLIENT_CA_BUNDLE", L"APPDATA"};
    std::vector<std::wstring> drop;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const wchar_t* eq = wcschr(p + 1, L'=');  // a leading '=' is a drive-cwd entry
            if (!eq) continue;
            std::wstring name(p, eq);
            std::wstring up = name;
            for (auto& c : up) c = static_cast<wchar_t>(towupper(c));
            bool hit = false;
            for (const wchar_t* pre : kPrefixes) hit = hit || up.rfind(pre, 0) == 0;
            for (const wchar_t* n : kNames) hit = hit || up == n;
            if (hit) drop.push_back(name);
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& n : drop) SetEnvironmentVariableW(n.c_str(), nullptr);
    const std::wstring home = cache + L"\\home", tmp = cache + L"\\tmp";
    SetEnvironmentVariableW(L"HOME", home.c_str());
    SetEnvironmentVariableW(L"APPDATA", (home + L"\\appdata").c_str());  // yt-dlp's user config and plugin folders
    SetEnvironmentVariableW(L"XDG_CONFIG_HOME", home.c_str());
    SetEnvironmentVariableW(L"XDG_CACHE_HOME", home.c_str());
    SetEnvironmentVariableW(L"TEMP", tmp.c_str());  // yt-dlp unpacks itself here: must be writable at Low integrity
    SetEnvironmentVariableW(L"TMP", tmp.c_str());
    SetEnvironmentVariableW(L"DENO_DIR", (cache + L"\\deno").c_str());
    SetEnvironmentVariableW(L"DENO_NO_UPDATE_CHECK", L"1");
    SetEnvironmentVariableW(L"DENO_NO_PROMPT", L"1");
    SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE", L"1");
    SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
}

void ApplyMitigations() {
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ep{};
    ep.DisableExtensionPoints = 1;  // no AppInit DLLs, IMEs or other legacy injection points
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ep, sizeof ep)) HLOG("mitigation: extension points: %s", WinError(GetLastError()).c_str());
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY il{};
    // No NoLowMandatoryLabelImages: children inherit the policy, and yt-dlp (PyInstaller) must load the Python DLL it
    // unpacks into the Low TEMP folder.
    il.NoRemoteImages = 1;
    il.PreferSystem32Images = 1;
    if (!SetProcessMitigationPolicy(ProcessImageLoadPolicy, &il, sizeof il)) HLOG("mitigation: image load: %s", WinError(GetLastError()).c_str());
}

void JoinOwnJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        HLOG("job: own job not set up: %s", WinError(GetLastError()).c_str());
        if (job) CloseHandle(job);
        return;
    }
    // The handle stays open for the process's life; closing it at exit kills every child left.
}

bool WriteRootBundle(const std::wstring& path, int* count) {
    *count = 0;
    // Read-only: CertOpenSystemStore asks for write access, which a Low integrity token does not get.
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG, L"ROOT");
    if (!store) {
        HLOG("roots: %s", WinError(GetLastError()).c_str());
        return false;
    }
    std::string pem;
    PCCERT_CONTEXT c = nullptr;
    while ((c = CertEnumCertificatesInStore(store, c)) != nullptr) {
        DWORD n = 0;
        if (!CryptBinaryToStringA(c->pbCertEncoded, c->cbCertEncoded, CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCR, nullptr, &n)) continue;
        std::string one(n, '\0');
        if (!CryptBinaryToStringA(c->pbCertEncoded, c->cbCertEncoded, CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCR, one.data(), &n)) continue;
        one.resize(n);
        pem += one;
        ++*count;
    }
    CertCloseStore(store, 0);
    if (!*count) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, pem.data(), static_cast<DWORD>(pem.size()), &w, nullptr) && w == pem.size();
    CloseHandle(h);
    return ok;
}

HANDLE HoldLock(const std::wstring& cache) {
    HANDLE h = CreateFileW((cache + L"\\lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) HLOG("lock: %s", WinError(GetLastError()).c_str());
    return h;
}

bool DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                // a junction or link: remove the link itself, never what it points at
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
                else DeleteFileW(p.c_str());
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(p);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

namespace {
bool InstanceName(const std::wstring& n) {  // <pid>-<hex>
    const size_t dash = n.find(L'-');
    if (dash == 0 || dash == std::wstring::npos || dash + 1 >= n.size() || n.size() > 64) return false;
    for (size_t i = 0; i < n.size(); ++i) {
        const wchar_t c = n[i];
        if (i == dash) continue;
        if (i < dash ? !(c >= L'0' && c <= L'9') : !iswxdigit(c)) return false;
    }
    return true;
}
}  // namespace

void SweepDeadInstances(const std::wstring& cache) {
    const size_t cut = cache.find_last_of(L'\\');
    if (cut == std::wstring::npos || cut < 3) return;
    const std::wstring parent = cache.substr(0, cut), self = cache.substr(cut + 1);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((parent + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    int swept = 0;
    do {
        const std::wstring n = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            _wcsicmp(n.c_str(), self.c_str()) == 0 || !InstanceName(n))
            continue;
        const std::wstring dir = parent + L"\\" + n;
        // A live helper holds its lock without delete sharing; a dead one's lock opens for delete.
        HANDLE l = CreateFileW((dir + L"\\lock").c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool dead = l != INVALID_HANDLE_VALUE;
        if (dead) {
            CloseHandle(l);
        } else if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            // no lock at all: a helper that died before creating it; only if the folder is over an hour old
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            const auto t = [](FILETIME x) { return (static_cast<uint64_t>(x.dwHighDateTime) << 32) | x.dwLowDateTime; };
            dead = t(now) - t(fd.ftLastWriteTime) > 36000000000ull;
        }
        if (dead && DeleteTree(dir)) ++swept;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (swept) HLOG("sweep: removed %d dead instance folder(s)", swept);
}

RunResult Run(const std::vector<std::wstring>& argv, const std::wstring& cwd, uint32_t timeoutMs, size_t maxOut, size_t maxErr) {
    RunResult r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        r.why = "CreatePipe: " + WinError(GetLastError());
        for (HANDLE h : {outR, outW, errR, errW, nul})
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return r;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    // Only these three handles are inherited, nothing else the helper holds (the pipe, the sections, the lock).
    HANDLE inherit[3] = {nul, outW, errW};
    SIZE_T attrBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
    std::vector<uint8_t> attrBuf(attrBytes);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrBytes);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = errW;
    si.lpAttributeList = attrs;
    std::wstring cmd = CommandLine(argv);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(argv[0].c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    const DWORD startErr = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(outW);
    CloseHandle(errW);
    CloseHandle(nul);
    if (!ok) {
        r.why = "CreateProcess: " + WinError(startErr);
        CloseHandle(outR);
        CloseHandle(errR);
        return r;
    }
    r.started = true;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) {
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        if (!AssignProcessToJobObject(job, pi.hProcess)) HLOG("run: job assign failed: %s", WinError(GetLastError()).c_str());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto drain = [](HANDLE h, std::string* dst, size_t cap) {
        char buf[16384];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) {
            if (dst->size() < cap) dst->append(buf, got < cap - dst->size() ? got : cap - dst->size());
        }
    };
    std::thread to([&] { drain(outR, &r.out, maxOut); });
    std::thread te([&] { drain(errR, &r.err, maxErr); });
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
        r.timedOut = true;
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = static_cast<int>(code);
    }
    if (job) TerminateJobObject(job, 1);  // anything the program left running (it would hold the pipes open)
    to.join();
    te.join();
    CloseHandle(outR);
    CloseHandle(errR);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return r;
}
}  // namespace holo::sandbox
