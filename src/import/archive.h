#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "import/recipe.h"

// The verified zip, read through the recipe's member allowlist. Members outside it are never decompressed; the ones
// on it pass the name, method, attribute, size and ratio checks before a byte is inflated.
namespace melange::import {
constexpr size_t kMaxEntries = 4000;
constexpr uint64_t kMaxTotalRead = 192ull << 20;

enum class Kind { Registry, Title, Descriptor, Xan, Txt, Hmp, Preview };
uint64_t KindCap(Kind k);

struct Member {
    std::string name;   // full name in the zip
    std::string rel;    // under reader.root
    Kind kind = Kind::Xan;
    unsigned index = 0;
    uint64_t size = 0, compSize = 0;
};

// The name rules for a member we read: relative, no "..", drive, backslash, control or reserved names, no hidden
// segment, at most 180 bytes and 8 segments, characters A-Z a-z 0-9 . _ - space ( ).
bool CheckMemberName(const std::string& name, std::string* why);

class Archive {
  public:
    Archive();
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    // Opens the file, reads the central directory once and checks every allowlisted member.
    bool Open(const std::wstring& path, const Reader& reader, std::string* err);
    // Allowlisted members by lower-case `rel`.
    const std::map<std::string, Member>& Members() const { return members_; }
    const Member* Find(const std::string& rel) const;   // case-insensitive
    // Inflates one member into memory: bounded by its declared size and its kind's cap, CRC checked, no executable
    // magic. Counts against the total read cap.
    bool Read(const Member& m, std::vector<uint8_t>* out, std::string* err, const std::atomic<bool>* cancel = nullptr);
    uint64_t TotalRead() const { return totalRead_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::map<std::string, Member> members_;
    uint64_t totalRead_ = 0;
};
}  // namespace melange::import
