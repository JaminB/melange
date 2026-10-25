#pragma once
// The core's side of a player's frame section (shm_logic.h) and the security every helper-facing object gets: a DACL
// for the current user only, plus a Low mandatory label when the helper runs at Low integrity (Critique S2), so the
// sandboxed helper can open exactly these objects and nothing else of ours. Game-free.
#include <cstdint>
#include <string>

#include "media/shm_logic.h"

namespace melange::media::shmio {
// A self-relative security descriptor in a SECURITY_ATTRIBUTES (not inheritable).
class Security {
public:
    Security() = default;
    ~Security();
    Security(const Security&) = delete;
    Security& operator=(const Security&) = delete;
    // D:P(A;;GA;;;<me>)(A;;GA;;;SY), and S:(ML;;NW;;;LW) when `lowLabel`.
    bool Build(bool lowLabel, std::string* err);
    void* Attributes() { return sd_ ? &sa_ : nullptr; }  // SECURITY_ATTRIBUTES*

private:
    void* sd_ = nullptr;
    struct {
        unsigned long nLength;
        void* lpSecurityDescriptor;
        int bInheritHandle;
    } sa_{};
};

// "Local\melange-holo-<pid>-<hex>-<id>" (pipe_codec's ShmName accepts exactly this alphabet).
std::string SectionName(uint32_t pid, const std::string& hex, uint32_t playerId);

// One player's section: created by the core with its own size (never opened: a name that already exists is refused,
// so nothing can pre-create it), mapped read-write (the reader writes `reading`), header initialised.
class Section {
public:
    Section() = default;
    ~Section();
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
    bool Create(const std::string& name, bool lowLabel, std::string* err);
    bool Ok() const { return base_ != nullptr; }
    const std::string& Name() const { return name_; }
    void* Base() const { return base_; }
    shm::Reader& Reader() { return *reader_; }

private:
    void* map_ = nullptr;   // HANDLE
    void* base_ = nullptr;
    shm::Reader* reader_ = nullptr;
    std::string name_;
};
}  // namespace melange::media::shmio
