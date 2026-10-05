#include "store/fetch.h"

#include <windows.h>

#include <winhttp.h>

#include <mutex>
#include <vector>

#include "store/index.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace melange::store::fetch {
namespace {
std::mutex g_mx;
HINTERNET g_active = nullptr;

std::string Mb(uint64_t n) { return std::to_string((n + (1 << 20) - 1) >> 20) + " MiB"; }

std::string WinErr(const char* what, DWORD e) {
    switch (e) {
        case ERROR_WINHTTP_TIMEOUT: return std::string(what) + ": timed out";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return std::string(what) + ": the server name could not be resolved (offline?)";
        case ERROR_WINHTTP_CANNOT_CONNECT: return std::string(what) + ": could not connect";
        case ERROR_WINHTTP_SECURE_FAILURE: return std::string(what) + ": the secure connection failed (certificate or TLS)";
        case ERROR_WINHTTP_OPERATION_CANCELLED:
        case ERROR_INVALID_HANDLE: return "cancelled";
        case ERROR_WINHTTP_REDIRECT_FAILED: return std::string(what) + ": refused redirect";
        default: break;
    }
    return std::string(what) + " failed (error " + std::to_string(e) + ")";
}

bool Cancelled(const Options& o) { return o.cancel && o.cancel->load(); }

bool ReadFileUrl(const std::string& url, Sink& sink, const Options& o, std::string* err) {
    std::wstring path;
    if (!FileUrlToPath(url, &path)) {
        *err = "not a file:/// path";
        return false;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *err = "cannot open " + url;
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) != 0;
    if (!ok) *err = "cannot read " + url;
    if (ok && o.cap && static_cast<uint64_t>(size.QuadPart) > o.cap) {
        *err = "the file is larger than the " + Mb(o.cap) + " limit";
        ok = false;
    }
    std::vector<char> buf(64 * 1024);
    uint64_t got = 0;
    while (ok) {
        if (Cancelled(o)) {
            *err = "cancelled";
            ok = false;
            break;
        }
        DWORD n = 0;
        if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
            *err = "cannot read " + url;
            ok = false;
            break;
        }
        if (!n) break;
        got += n;
        if (o.cap && got > o.cap) {
            *err = "the file is larger than the " + Mb(o.cap) + " limit";
            ok = false;
            break;
        }
        if (!sink.Write(buf.data(), n)) {
            *err = "cannot write the download";
            ok = false;
            break;
        }
        if (o.progress) o.progress(got, static_cast<uint64_t>(size.QuadPart));
    }
    CloseHandle(f);
    return ok;
}

struct Handles {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr, own = nullptr;
    ~Handles() {
        if (own) WinHttpCloseHandle(own);
        {
            std::lock_guard lk(g_mx);
            if (request && g_active == request) {
                WinHttpCloseHandle(request);
                g_active = nullptr;
            }
        }
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

bool GetHttps(const std::string& url, Sink& sink, const Options& o, std::string* err) {
    const ULONGLONG start = GetTickCount64();
    const int wn = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring wurl(static_cast<size_t>(wn > 0 ? wn : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), wn);
    wurl.resize(wcslen(wurl.c_str()));

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        *err = "not an https:// URL";
        return false;
    }
    std::wstring object(path, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    std::wstring agent(o.userAgent.begin(), o.userAgent.end());
    Handles h;
    h.session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session)
        h.session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) {
        *err = WinErr("WinHttpOpen", GetLastError());
        return false;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(h.session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(h.session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            *err = "TLS 1.2 is not available on this system";
            return false;
        }
    }
    WinHttpSetTimeouts(h.session, 10000, 10000, 10000, static_cast<int>(o.stallMs));
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP, maxRedirects = 5;
    WinHttpSetOption(h.session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    WinHttpSetOption(h.session, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));

    h.connect = WinHttpConnect(h.session, host, uc.nPort, 0);
    if (!h.connect) {
        *err = WinErr("connect", GetLastError());
        return false;
    }
    const wchar_t* accept[] = {L"*/*", nullptr};
    HINTERNET req = WinHttpOpenRequest(h.connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_SECURE);
    if (!req) {
        *err = WinErr("request", GetLastError());
        return false;
    }
    if (o.registerActive) {
        std::lock_guard lk(g_mx);
        g_active = req;
        h.request = req;
    } else {
        h.own = req;
    }
    DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    WinHttpSetOption(req, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));
    if (Cancelled(o)) {
        *err = "cancelled";
        return false;
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        *err = Cancelled(o) ? "cancelled" : WinErr("download", GetLastError());
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        *err = "the server answered HTTP " + std::to_string(status);
        return false;
    }
    wchar_t finalUrl[2048] = {};
    DWORD fl = sizeof(finalUrl);
    if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, finalUrl, &fl) && _wcsnicmp(finalUrl, L"https://", 8) != 0) {
        *err = "refused a redirect away from https://";
        return false;
    }
    DWORD contentLength = 0;
    len = sizeof(contentLength);
    const bool haveLength = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                                WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &len, WINHTTP_NO_HEADER_INDEX) != 0;
    if (haveLength && o.cap && contentLength > o.cap) {
        *err = "the file is larger than the " + Mb(o.cap) + " limit";
        return false;
    }
    std::vector<char> buf(64 * 1024);
    uint64_t got = 0;
    for (;;) {
        if (Cancelled(o)) {
            *err = "cancelled";
            return false;
        }
        if (o.totalMs && GetTickCount64() - start > o.totalMs) {
            *err = "download: timed out";
            return false;
        }
        DWORD n = 0;
        if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
            *err = Cancelled(o) ? "cancelled" : WinErr("download", GetLastError());
            return false;
        }
        if (!n) break;
        got += n;
        if (o.cap && got > o.cap) {
            *err = "the file is larger than the " + Mb(o.cap) + " limit";
            return false;
        }
        if (!sink.Write(buf.data(), n)) {
            *err = "cannot write the download";
            return false;
        }
        if (o.progress) o.progress(got, haveLength ? contentLength : 0);
    }
    return true;
}
}  // namespace

bool Get(const std::string& url, Sink& sink, const Options& o, std::string* err) {
    switch (SchemeOf(url)) {
        case Scheme::Https: return GetHttps(url, sink, o, err);
        case Scheme::File: return ReadFileUrl(url, sink, o, err);
        case Scheme::Other: break;
    }
    *err = "url scheme not allowed";
    return false;
}

void CancelActive() {
    std::lock_guard lk(g_mx);
    if (g_active) {
        WinHttpCloseHandle(g_active);
        g_active = nullptr;
    }
}
}  // namespace melange::store::fetch
