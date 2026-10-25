#pragma once
// Small helpers every part of holo-media uses: UTF-8 <-> UTF-16, the QueryPerformanceCounter clock in ms (the same
// clock the core reads, since QPC is machine-wide), and Windows command-line quoting.
#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace holo {
inline std::string Utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// QueryPerformanceCounter in ms: what prop.qpc carries (peerbus::NowMs reads the same clock in the game).
inline double QpcMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

// One argument quoted the way CommandLineToArgvW and the MSVC CRT split it back (backslashes before a quote doubled).
inline std::wstring QuoteArg(std::wstring_view a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(a);
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
inline std::wstring CommandLine(const std::vector<std::wstring>& argv) {
    std::wstring s;
    for (const auto& a : argv) {
        if (!s.empty()) s.push_back(L' ');
        s += QuoteArg(a);
    }
    return s;
}

// A Windows error as "text (code)", for the log and error messages.
inline std::string WinError(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string s = buf ? Utf8(buf) : std::string();
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s + " (" + std::to_string(code) + ")";
}
}  // namespace holo
