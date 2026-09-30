#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
// Oasis: the local web app served on 127.0.0.1. Modules add channels (server -> browser pushes), RPC methods and
// web panels. "Any thread" is marked; everything else is main-thread-only.
namespace melange::oasis {
constexpr int kProtocol = 1;

bool Enabled();                       // [Oasis] Enabled and the core loaded
bool Running();                       // the socket is listening; any thread
bool Start();                         // idempotent; any thread (queued to the server thread)
std::string Url();                    // "http://127.0.0.1:<port>/?k=<token>", "" when not running; any thread
int Clients();                        // authenticated WS clients; any thread

// Channels: server -> client pushes. Names: [a-z0-9._-]{1,48}; "mod.<modid>.<x>" for mods. Core names are reserved.
enum class Overflow : uint8_t { DropOldest, Coalesce };  // Coalesce keeps only the latest message per client
struct ChannelOptions {
    Overflow overflow = Overflow::DropOldest;
    uint32_t maxQueueKB = 256;        // per client, on top of the 2 MB connection cap
    bool mainThreadSubscribe = false; // OnSubscribe callbacks on the main thread (default: server thread)
};
using ChannelId = uint32_t;           // 0 = failure
ChannelId AddChannel(const char* name, const ChannelOptions& opt = {});  // any thread
void RemoveChannel(ChannelId ch);                                         // any thread
bool HasSubscribers(ChannelId ch);    // any thread, lock-free; producers skip work when false
uint32_t SubscriberCount(ChannelId ch);  // any thread; exact count (wum.web.channel():subscribers())
// Copies `jsonData` (one JSON value) into every subscribed client's queue. Any thread. Never blocks.
// Returns false when nobody is subscribed or the channel is gone (not an error).
bool Publish(ChannelId ch, std::string_view jsonData);
// As Publish, to one subscribed client only (per-client filters). Any thread. Never blocks.
bool PublishTo(ChannelId ch, int client, std::string_view jsonData);
using SubscribeFn = void (*)(ChannelId ch, int client, std::string_view filterJson, bool subscribed, void* user);
int OnSubscribe(ChannelId ch, SubscribeFn fn, void* user);  // filters are opaque to the core; the owner applies them

// RPC methods. Names: "<area>.<verb>", "mod.<modid>.<verb>" for mods.
enum RpcFlags : uint32_t {
    kRpcNone = 0,
    kRpcServerThread = 1,  // run on the server thread (must not touch the game); default: main thread, Frame event
    kRpcMutating = 2,      // changes something (ini, mods, captures); refused when [Oasis] ReadOnly=1
    kRpcGameOnly = 4,      // listed as unavailable by the standalone server
};
struct Call {
    std::string_view method, paramsJson;  // params: a JSON object or "{}"
    int client;
};
struct Result {
    bool ok = true;
    std::string json = "null";           // ok: one JSON value
    int code = 0;                         // !ok: an error code (-32602 bad params, -32000 refused, -32001 not in a match, ...)
    std::string message;
};
using RpcFn = void (*)(const Call& c, Result& r, void* user);  // must return within one frame's budget
int AddMethod(const char* name, RpcFn fn, void* user, uint32_t flags = kRpcNone);  // any thread
void RemoveMethod(int handle);                                                      // any thread

// Web panels contributed by C++ modules: a folder of static files, loaded in a sandboxed iframe.
int AddWebPanel(const char* id, const char* title, const wchar_t* dir, const char* entry = "index.html");  // any thread
void RemoveWebPanel(int handle);

// Called on the server thread when a client's connection has ended (connId = Call::client). Any thread; returns a
// handle (0 = failure).
using ClientClosedFn = void (*)(uint64_t connId, void* user);
int OnClientClosed(ClientClosedFn fn, void* user);
void RemoveOnClientClosed(int handle);

struct Stats { uint32_t clients, channels, methods; uint64_t framesOut, bytesOut, bytesIn, dropped, authFailures, rpcCalls; };
Stats GetStats();  // any thread
}
