#include "store/fetch.h"

#include <windows.h>

#include <winhttp.h>

#include <cstring>
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
    uint64_t got = 0;
    if (ok && o.resumeFrom && o.resumeFrom < static_cast<uint64_t>(size.QuadPart)) {
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(o.resumeFrom);
        if (SetFilePointerEx(f, at, nullptr, FILE_BEGIN)) got = o.resumeFrom;
    }
    if (ok && !sink.Start(got, static_cast<uint64_t>(size.QuadPart))) {
        *err = "cannot write the download";
        ok = false;
    }
    std::vector<char> buf(64 * 1024);
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

// The session and the connection for an https:// URL, shared by Get and Send: the system proxy (WPAD, then the
// default), TLS 1.2/1.3, the timeouts and the session's redirect policy. `object` gets the path and query.
bool OpenHttps(const std::string& url, const Options& o, DWORD redirectPolicy, Handles* h, std::wstring* object, std::string* err) {
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
    object->assign(path, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) object->append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    std::wstring agent(o.userAgent.begin(), o.userAgent.end());
    h->session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session)
        h->session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) {
        *err = WinErr("WinHttpOpen", GetLastError());
        return false;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(h->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(h->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            *err = "TLS 1.2 is not available on this system";
            return false;
        }
    }
    WinHttpSetTimeouts(h->session, 10000, 10000, 10000, static_cast<int>(o.stallMs));
    DWORD redirect = redirectPolicy, maxRedirects = 5;
    WinHttpSetOption(h->session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    WinHttpSetOption(h->session, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));

    h->connect = WinHttpConnect(h->session, host, uc.nPort, 0);
    if (!h->connect) {
        *err = WinErr("connect", GetLastError());
        return false;
    }
    return true;
}

// The request handle: registered for CancelActive, or owned by `h` alone (Options::registerActive = false).
void Own(const Options& o, Handles* h, HINTERNET req) {
    if (o.registerActive) {
        std::lock_guard lk(g_mx);
        g_active = req;
        h->request = req;
    } else {
        h->own = req;
    }
}

bool GetHttps(const std::string& url, Sink& sink, const Options& o, std::string* err) {
    const ULONGLONG start = GetTickCount64();
    Handles h;
    std::wstring object;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP, maxRedirects = 5;
    if (!OpenHttps(url, o, redirect, &h, &object, err)) return false;
    const wchar_t* accept[] = {L"*/*", nullptr};
    HINTERNET req = WinHttpOpenRequest(h.connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_SECURE);
    if (!req) {
        *err = WinErr("request", GetLastError());
        return false;
    }
    Own(o, &h, req);
    DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    WinHttpSetOption(req, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));
    if (Cancelled(o)) {
        *err = "cancelled";
        return false;
    }
    std::wstring range;
    if (o.resumeFrom) range = L"Range: bytes=" + std::to_wstring(o.resumeFrom) + L"-";
    if (!WinHttpSendRequest(req, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(),
                            range.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        *err = Cancelled(o) ? "cancelled" : WinErr("download", GetLastError());
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    uint64_t offset = 0, rangeTotal = 0;
    if (status == 206 && o.resumeFrom) {
        // Only the exact range asked for: anything else would splice the wrong bytes onto the kept part.
        wchar_t cr[128] = {};
        DWORD crn = sizeof(cr);
        uint64_t first = 0, last = 0;
        std::string crs;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, cr, &crn, WINHTTP_NO_HEADER_INDEX))
            for (const wchar_t* c = cr; *c; ++c) crs.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
        if (!ParseContentRange(crs, &first, &last, &rangeTotal) || first != o.resumeFrom) {
            *err = "the server answered a different range (" + crs + ")";
            return false;
        }
        offset = first;
    } else if (status != 200) {
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
    const uint64_t total = rangeTotal ? rangeTotal : haveLength ? offset + contentLength : 0;
    if (o.cap && (total > o.cap || (haveLength && offset + contentLength > o.cap))) {
        *err = "the file is larger than the " + Mb(o.cap) + " limit";
        return false;
    }
    if (!sink.Start(offset, total)) {
        *err = "cannot write the download";
        return false;
    }
    std::vector<char> buf(64 * 1024);
    uint64_t got = offset;
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
        if (o.progress) o.progress(got, total);
    }
    return true;
}

// A header value as ASCII (the ones read here are ASCII; anything else becomes '?').
std::string QueryHeader(HINTERNET req, DWORD level, const wchar_t* name) {
    wchar_t buf[256] = {};
    DWORD n = sizeof(buf) - sizeof(wchar_t);
    std::string s;
    if (WinHttpQueryHeaders(req, level, name, buf, &n, WINHTTP_NO_HEADER_INDEX))
        for (const wchar_t* c = buf; *c; ++c) s.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    return s;
}

bool HeaderTokenOk(const std::string& s, bool value) {
    if (s.empty() && !value) return false;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == 0 || c >= 0x80) return false;
        if (!value && (c <= 0x20 || c == ':')) return false;
    }
    return true;
}

// Zeroes the wide copy of the request headers (an Authorization header) when SendHttps returns.
struct WipeOnExit {
    std::wstring& s;
    ~WipeOnExit() {
        if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    }
};

bool SendHttps(const std::string& url, const HttpRequest& rq, HttpResponse* out, const Options& o, std::string* err) {
    const ULONGLONG start = GetTickCount64();
    *out = HttpResponse{};
    std::wstring headers;
    const WipeOnExit wipeHeaders{headers};
    std::wstring method;
    for (char c : rq.method) {
        if (c < 'A' || c > 'Z') {
            *err = "bad method";
            return false;
        }
        method.push_back(static_cast<wchar_t>(c));
    }
    if (method.empty() || method.size() > 16) {
        *err = "bad method";
        return false;
    }
    size_t headerChars = 0;
    for (const auto& [k, v] : rq.headers) {
        if (!HeaderTokenOk(k, false) || !HeaderTokenOk(v, true)) {
            *err = "bad header";
            return false;
        }
        headerChars += k.size() + v.size() + 4;
    }
    headers.reserve(headerChars);  // appended in place: no temporary or reallocated copy of a token is left behind
    for (const auto& [k, v] : rq.headers) {
        headers.append(k.begin(), k.end());
        headers.append(L": ");
        headers.append(v.begin(), v.end());
        headers.append(L"\r\n");
    }
    if (rq.body.size() > 0x7fffffff) {
        *err = "the request body is too large";
        return false;
    }
    Handles h;
    std::wstring object;
    // Redirects are never followed, so an Authorization header cannot leave the host it was meant for.
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!OpenHttps(url, o, redirect, &h, &object, err)) return false;
    const wchar_t* accept[] = {L"*/*", nullptr};
    HINTERNET req = WinHttpOpenRequest(h.connect, method.c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_SECURE);
    if (!req) {
        *err = WinErr("request", GetLastError());
        return false;
    }
    Own(o, &h, req);
    DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    if (Cancelled(o)) {
        *err = "cancelled";
        return false;
    }
    const DWORD bodyLen = static_cast<DWORD>(rq.body.size());
    if (!WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L),
                            bodyLen ? const_cast<char*>(rq.body.data()) : WINHTTP_NO_REQUEST_DATA, bodyLen, bodyLen, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        *err = Cancelled(o) ? "cancelled" : WinErr("request", GetLastError());
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    out->status = static_cast<int>(status);
    out->retryAfter = QueryHeader(req, WINHTTP_QUERY_CUSTOM, L"Retry-After");
    out->contentType = QueryHeader(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX);
    DWORD contentLength = 0;
    len = sizeof(contentLength);
    const bool haveLength = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                                WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &len, WINHTTP_NO_HEADER_INDEX) != 0;
    if (o.cap && haveLength && contentLength > o.cap) {
        *err = "the answer is larger than the " + Mb(o.cap) + " limit";
        return false;
    }
    // The body is read for every status (an API's error answer is JSON too).
    std::vector<char> buf(64 * 1024);
    for (;;) {
        if (Cancelled(o)) {
            *err = "cancelled";
            return false;
        }
        if (o.totalMs && GetTickCount64() - start > o.totalMs) {
            *err = "request: timed out";
            return false;
        }
        DWORD n = 0;
        if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
            *err = Cancelled(o) ? "cancelled" : WinErr("request", GetLastError());
            return false;
        }
        if (!n) break;
        if (o.cap && out->body.size() + n > o.cap) {
            *err = "the answer is larger than the " + Mb(o.cap) + " limit";
            return false;
        }
        out->body.append(buf.data(), n);
    }
    return true;
}
}  // namespace

bool Send(const std::string& url, const HttpRequest& req, HttpResponse* out, const Options& o, std::string* err) {
    if (SchemeOf(url) != Scheme::Https) {
        *err = "url scheme not allowed";
        return false;
    }
    return SendHttps(url, req, out, o, err);
}

bool Get(const std::string& url, Sink& sink, const Options& o, std::string* err) {
    switch (SchemeOf(url)) {
        case Scheme::Https: return GetHttps(url, sink, o, err);
        case Scheme::File: return ReadFileUrl(url, sink, o, err);
        case Scheme::Other: break;
    }
    *err = "url scheme not allowed";
    return false;
}

bool ParseContentRange(std::string_view v, uint64_t* first, uint64_t* last, uint64_t* total) {
    auto num = [&](size_t& i, uint64_t* out) {
        const size_t from = i;
        uint64_t n = 0;
        for (; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i) {
            if (i - from >= 19) return false;
            n = n * 10 + static_cast<uint64_t>(v[i] - '0');
        }
        *out = n;
        return i > from;
    };
    if (v.size() < 6 || _strnicmp(v.data(), "bytes ", 6) != 0) return false;
    size_t i = 6;
    while (i < v.size() && v[i] == ' ') ++i;
    if (!num(i, first) || i >= v.size() || v[i++] != '-' || !num(i, last) || i >= v.size() || v[i++] != '/') return false;
    if (i < v.size() && v[i] == '*' && i + 1 == v.size()) {
        *total = 0;
    } else if (!num(i, total) || i != v.size() || *last >= *total) {
        return false;
    }
    return *first <= *last;
}

void CancelActive() {
    std::lock_guard lk(g_mx);
    if (g_active) {
        WinHttpCloseHandle(g_active);
        g_active = nullptr;
    }
}
}  // namespace melange::store::fetch
