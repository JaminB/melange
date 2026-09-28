#include <windows.h>

#include <cstdio>

#include "oasis/core/files.h"

namespace melange::oasis::core {
namespace {
std::wstring Widen(std::string_view s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

class DirSource final : public Files {
  public:
    explicit DirSource(std::wstring dir) : dir_(std::move(dir)) {}

    bool Get(std::string_view path, std::string* body, std::string* etag, bool* gzipped) override {
        *gzipped = false;
        std::wstring f = dir_ + L"\\" + Widen(path);
        for (auto& c : f)
            if (c == L'/') c = L'\\';
        HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        bool ok = GetFileInformationByHandle(h, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                  info.nFileSizeHigh == 0 && info.nFileSizeLow <= (64u << 20);
        if (ok) {
            body->resize(info.nFileSizeLow);
            DWORD got = 0;
            ok = info.nFileSizeLow == 0 || (ReadFile(h, body->data(), info.nFileSizeLow, &got, nullptr) && got == info.nFileSizeLow);
            char tag[64];
            snprintf(tag, sizeof tag, "\"%lx%08lx-%lx\"", info.ftLastWriteTime.dwHighDateTime,
                     info.ftLastWriteTime.dwLowDateTime, info.nFileSizeLow);
            *etag = tag;
        }
        CloseHandle(h);
        return ok;
    }

  private:
    std::wstring dir_;
};
}  // namespace

std::unique_ptr<Files> DirFiles(const std::wstring& dir) { return std::make_unique<DirSource>(dir); }
}  // namespace melange::oasis::core
