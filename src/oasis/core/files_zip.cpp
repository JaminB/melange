#include <miniz.h>

#include <cstdio>
#include <map>
#include <mutex>

#include "oasis/core/files.h"

namespace melange::oasis::core {
namespace {
struct Entry { std::string body, etag; };

class ZipSource final : public Files {
  public:
    bool Init(const void* data, size_t size) { return mz_zip_reader_init_mem(&zip_, data, size, 0) != 0; }
    ~ZipSource() override { mz_zip_reader_end(&zip_); }

    bool Get(std::string_view path, std::string* body, std::string* etag, bool* gzipped) override {
        std::lock_guard lk(mx_);
        const std::string p(path);
        if (*gzipped) {
            if (const Entry* e = Load(p + ".gz")) {
                *body = e->body;
                *etag = e->etag;
                return true;
            }
        }
        *gzipped = false;
        const Entry* e = Load(p);
        if (!e) return false;
        *body = e->body;
        *etag = e->etag;
        return true;
    }

  private:
    const Entry* Load(const std::string& name) {
        if (auto it = cache_.find(name); it != cache_.end()) return &it->second;
        const int idx = mz_zip_reader_locate_file(&zip_, name.c_str(), nullptr, 0);
        mz_zip_archive_file_stat st;
        if (idx < 0 || !mz_zip_reader_file_stat(&zip_, static_cast<mz_uint>(idx), &st) || st.m_is_directory) return nullptr;
        size_t n = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip_, static_cast<mz_uint>(idx), &n, 0);
        if (!p) return nullptr;
        Entry e;
        e.body.assign(static_cast<const char*>(p), n);
        mz_free(p);
        char tag[48];
        snprintf(tag, sizeof tag, "\"%08x-%zx\"", static_cast<unsigned>(st.m_crc32), n);
        e.etag = tag;
        return &cache_.emplace(name, std::move(e)).first->second;
    }

    mz_zip_archive zip_{};
    std::mutex mx_;
    std::map<std::string, Entry> cache_;
};
}  // namespace

std::unique_ptr<Files> ZipFiles(const void* data, size_t size) {
    auto z = std::make_unique<ZipSource>();
    if (!data || !size || !z->Init(data, size)) return nullptr;
    return z;
}

std::string ZipEntryText(const void* data, size_t size, const char* name) {
    mz_zip_archive zip{};
    if (!data || !mz_zip_reader_init_mem(&zip, data, size, 0)) return {};
    size_t n = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&zip, name, &n, 0);
    std::string s;
    if (p) {
        s.assign(static_cast<const char*>(p), n);
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
}  // namespace melange::oasis::core
