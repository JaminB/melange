#include "import/archive.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include "store/zipcheck.h"

namespace melange::import {
namespace {
std::string Lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

bool Fail(std::string* why, std::string_view name, const char* text) {
    if (why) *why = std::string(name.substr(0, 200)) + ": " + text;
    return false;
}

bool DeviceName(std::string_view seg) {
    std::string base = Lower(seg.substr(0, seg.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
    return base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9';
}

bool SegChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == ' ' || c == '-' || c == '(' || c == ')';
}

struct Sink {
    std::vector<uint8_t>* out = nullptr;
    uint64_t limit = 0;
    bool over = false, magic = false;
    const std::atomic<bool>* cancel = nullptr;
    bool cancelled = false;
};

size_t WriteCb(void* op, mz_uint64, const void* buf, size_t n) {
    auto* s = static_cast<Sink*>(op);
    if (s->cancel && s->cancel->load()) {
        s->cancelled = true;
        return 0;
    }
    if (s->out->size() + n > s->limit) {
        s->over = true;
        return 0;
    }
    const auto* b = static_cast<const uint8_t*>(buf);
    s->out->insert(s->out->end(), b, b + n);
    if (s->out->size() >= 4 && s->out->size() - n < 4 && store::zipcheck::ExecutableMagic(s->out->data(), 4)) {
        s->magic = true;
        return 0;
    }
    return n;
}
}  // namespace

uint64_t KindCap(Kind k) {
    switch (k) {
        case Kind::Registry: return 1ull << 20;
        case Kind::Title: return 1ull << 20;
        case Kind::Descriptor: return 64ull << 10;
        case Kind::Xan: return 4ull << 20;
        case Kind::Txt: return 64ull << 10;
        case Kind::Hmp: return 256ull << 10;
        case Kind::Preview: return 2ull << 20;
    }
    return 0;
}

bool CheckMemberName(const std::string& name, std::string* why) {
    if (name.empty()) return Fail(why, "(empty)", "empty name");
    if (name.size() > store::zipcheck::kMaxPathBytes) return Fail(why, name, "path longer than 180 bytes");
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f) return Fail(why, name, "control character");
    if (name.front() == '/' || name.front() == '\\') return Fail(why, name, "absolute path");
    if (name.find('\\') != std::string::npos) return Fail(why, name, "backslash in path");
    if (name.find(':') != std::string::npos) return Fail(why, name, "drive letter or ':' in path");
    if (name.back() == '/') return Fail(why, name, "folder entry");
    size_t p = 0, segs = 0;
    while (p <= name.size()) {
        size_t q = name.find('/', p);
        if (q == std::string::npos) q = name.size();
        const std::string_view s(name.data() + p, q - p);
        if (++segs > store::zipcheck::kMaxSegments) return Fail(why, name, "more than 8 folder levels");
        if (s.empty()) return Fail(why, name, "empty path segment");
        if (s == "." || s == "..") return Fail(why, name, "'.' or '..' in path");
        if (s.front() == '.') return Fail(why, name, "hidden file or folder");
        if (s.back() == '.' || s.back() == ' ') return Fail(why, name, "segment ends with a dot or a space");
        for (char c : s)
            if (!SegChar(c)) return Fail(why, name, "unexpected character in the name");
        if (DeviceName(s)) return Fail(why, name, "Windows device name");
        p = q + 1;
    }
    if (store::zipcheck::ExecutableName(name)) return Fail(why, name, "executable file type");
    return true;
}

struct Archive::Impl {
    mz_zip_archive z{};
    FILE* f = nullptr;
    bool open = false;
    ~Impl() {
        if (open) mz_zip_reader_end(&z);
        if (f) fclose(f);
    }
};

Archive::Archive() : impl_(std::make_unique<Impl>()) {}
Archive::~Archive() = default;

bool Archive::Open(const std::wstring& path, const Reader& rd, std::string* err) {
    impl_ = std::make_unique<Impl>();
    members_.clear();
    totalRead_ = 0;
    impl_->f = _wfopen(path.c_str(), L"rb");
    if (!impl_->f) {
        *err = "cannot open the zip";
        return false;
    }
    _fseeki64(impl_->f, 0, SEEK_END);
    const long long size = _ftelli64(impl_->f);
    _fseeki64(impl_->f, 0, SEEK_SET);
    if (size <= 0 || !mz_zip_reader_init_cfile(&impl_->z, impl_->f, static_cast<mz_uint64>(size), 0)) {
        *err = "not a valid zip file";
        return false;
    }
    impl_->open = true;
    const mz_uint n = mz_zip_reader_get_num_files(&impl_->z);
    if (n > kMaxEntries) {
        *err = "more than 4000 entries";
        return false;
    }
    const std::string root = rd.root + "/";
    auto kindOf = [&](const std::string& rel, Kind* k) {
        if (Lower(rel) == Lower(rd.registry)) return *k = Kind::Registry, true;
        for (const auto& t : rd.titles)
            if (Lower(rel) == Lower(t)) return *k = Kind::Title, true;
        if (rel.find('/') == std::string::npos && GlobMatch(rd.descriptors, rel)) return *k = Kind::Descriptor, true;
        for (const auto& m : rd.maps)
            if (GlobMatch(m, rel)) {
                const std::string l = Lower(rel);
                *k = l.ends_with(".xan") ? Kind::Xan : l.ends_with(".txt") ? Kind::Txt : Kind::Hmp;
                return true;
            }
        if (!rd.previews.empty() && GlobMatch(rd.previews, rel)) return *k = Kind::Preview, true;
        return false;
    };
    for (mz_uint i = 0; i < n; ++i) {
        char name[1024] = {};
        if (!mz_zip_reader_get_filename(&impl_->z, i, name, sizeof name)) {
            *err = "unreadable zip entry";
            return false;
        }
        const std::string nm = name;
        if (nm.size() <= root.size() || nm.compare(0, root.size(), root) != 0) continue;
        const std::string rel = nm.substr(root.size());
        Kind kind;
        if (!kindOf(rel, &kind)) continue;
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&impl_->z, i, &st)) {
            *err = nm + ": unreadable zip entry";
            return false;
        }
        if (!CheckMemberName(nm, err)) return false;
        if (st.m_is_encrypted || (st.m_bit_flag & 0x41)) return Fail(err, nm, "encrypted entry");
        if (st.m_method != 0 && st.m_method != 8) return Fail(err, nm, "compression method other than stored or deflate");
        if (st.m_is_directory || (st.m_external_attr & 0x10)) return Fail(err, nm, "folder attribute on a file");
        const uint32_t mode = st.m_external_attr >> 16;
        if ((st.m_version_made_by >> 8) == 3 && mode && (mode & 0170000) != 0100000) return Fail(err, nm, "symbolic link or special file");
        if (st.m_external_attr & 0x400) return Fail(err, nm, "reparse point");
        if (st.m_uncomp_size > KindCap(kind)) return Fail(err, nm, "larger than allowed for its type");
        if (st.m_uncomp_size > (64u << 10) && st.m_uncomp_size > st.m_comp_size * 100) return Fail(err, nm, "compression ratio above 100:1");
        Member m{nm, rel, kind, i, st.m_uncomp_size, st.m_comp_size};
        if (!members_.emplace(Lower(rel), std::move(m)).second) return Fail(err, nm, "duplicate name (names are case-insensitive)");
    }
    return true;
}

const Member* Archive::Find(const std::string& rel) const {
    auto it = members_.find(Lower(rel));
    return it == members_.end() ? nullptr : &it->second;
}

bool Archive::Read(const Member& m, std::vector<uint8_t>* out, std::string* err, const std::atomic<bool>* cancel) {
    out->clear();
    if (!impl_->open) {
        *err = "the zip is not open";
        return false;
    }
    if (totalRead_ + m.size > kMaxTotalRead) return Fail(err, m.name, "the import reads more than 192 MiB");
    out->reserve(static_cast<size_t>(m.size));
    Sink s;
    s.out = out;
    s.limit = std::min<uint64_t>(m.size, KindCap(m.kind));
    s.cancel = cancel;
    const bool ok = mz_zip_reader_extract_to_callback(&impl_->z, m.index, &WriteCb, &s, 0) != 0;
    if (s.cancelled) {
        *err = "cancelled";
        return false;
    }
    if (!s.magic && out->size() >= 2 && out->size() < 4 && store::zipcheck::ExecutableMagic(out->data(), out->size())) s.magic = true;
    if (s.magic) return Fail(err, m.name, "executable content");
    if (s.over) return Fail(err, m.name, "inflates past its declared size");
    if (!ok || out->size() != m.size) return Fail(err, m.name, "corrupt entry (size or CRC)");
    totalRead_ += m.size;
    return true;
}
}  // namespace melange::import
