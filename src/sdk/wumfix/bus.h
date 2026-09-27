#pragma once
#include <cstdint>
#include <string_view>
namespace wf::bus {
using MsgId = uint16_t;
constexpr MsgId kInvalidId = 0xffff;

enum class Path : uint8_t { Post, Deliver };

// A read-only view of an in-flight engine message. Valid ONLY during the callback (the engine's message arena
// is reset when dispatch depth returns to 0, see §1.2). Copy anything you keep.
struct MessageView {
    const uint8_t* raw;     // the Message object (+0 vtable, +4 id, +8 payload)
    MsgId id;
    uint32_t size;          // object bytes (header at raw-4 minus 4); 0 if the header looks bogus
    uintptr_t vtable;
    const char* name;       // registry name, "sys:0x1004", or "?8123"; never null; static lifetime
    const char* className;  // "ExplosionMessage", ... or "?" for unknown vtables; static lifetime
    Path path;
    int32_t handle;         // Deliver: target handle (-1 = root); Post: the post target (normally 7)
    bool fromPost;          // Deliver only: this object is currently inside a Post on the stack (fan-out copy)
    uint16_t depth;         // nesting depth of our hooks on this thread (1 = outermost)
    uintptr_t caller;       // return address into WormsMayhem.exe of the Post/Deliver call
    uint64_t seq;           // monotonically increasing per observed call
    uint64_t frame;         // wf::events::FrameCount()

    // Bounds-checked, SEH-guarded payload reads (return false past `size` or on fault).
    bool Read(uint32_t offset, void* out, uint32_t n) const;
    template <class T> bool Get(uint32_t offset, T& out) const { return Read(offset, &out, sizeof(T)); }
};

using Handler = void (*)(const MessageView& m, void* user);
using SubId = uint32_t;  // 0 = failure

// Synchronous, main thread, called BEFORE the engine processes the message. Handlers must not post, block,
// throw across the boundary, or keep `m`. A handler that faults 3 times is disabled (logged).
// Subscribe/Unsubscribe are safe from any thread and from inside a handler (takes effect for the next message).
SubId Subscribe(MsgId id, Path path, Handler fn, void* user = nullptr);
SubId SubscribeName(const char* name, Path path, Handler fn, void* user = nullptr);  // resolves lazily if the
                                                                                     // registry is not ready yet
SubId SubscribeAll(Path path, Handler fn, void* user = nullptr);  // every id; keep it cheap
void Unsubscribe(SubId id);

// Registry (read directly from *0x96d094 / *0x96d08c; never calls 0x690d44).
bool RegistryReady();                     // table pointer non-null and >= 1 name
MsgId IdOf(std::string_view name);        // kInvalidId if not registered
const char* NameOf(MsgId id);             // as MessageView::name
template <class F> void ForEachName(F&& f);  // f(MsgId, const char*) for every non-null slot (inline in header)
size_t Capacity();

// Typed payload decoders, keyed by message vtable. Writes JSON object members (no braces) into `out`.
struct JsonOut {  // minimal writer; implemented by C's jlog, declared here to keep B independent
    virtual void Int(const char* k, int64_t v) = 0;
    virtual void Uint(const char* k, uint64_t v) = 0;
    virtual void Hex(const char* k, uint64_t v) = 0;
    virtual void Float(const char* k, double v) = 0;
    virtual void Str(const char* k, std::string_view v) = 0;
    virtual void Vec3(const char* k, const float v[3]) = 0;
  protected:
    ~JsonOut() = default;
};
using Decoder = void (*)(const MessageView& m, JsonOut& out);
bool RegisterDecoder(uintptr_t vtable, const char* className, Decoder fn);  // replaces an existing one
bool Decode(const MessageView& m, JsonOut& out);  // false if no decoder for m.vtable

struct Stats { uint64_t posts, deliveries, handlerCalls, handlerFaults; double avgHookUs; };
Stats GetStats();
uint32_t CountOf(MsgId id, Path path);  // since start
bool Installed();                        // hooks live (false on unknown exe or when [EventBus] Enabled=0)
}
