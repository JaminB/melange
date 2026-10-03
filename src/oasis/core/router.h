#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "melange/oasis.h"
#include "oasis/core/server.h"

// The protocol (hello/welcome, sub/unsub, call/res/err, ev/drop) and the channel, method and panel registry.
// The registry lives for the whole process; clients exist while the server runs.
namespace melange::oasis::core {
enum Err : int {
    kErrEnvelope = -32600, kErrMethod = -32601, kErrParams = -32602, kErrRefused = -32000, kErrNotInMatch = -32001,
    kErrBusy = -32002, kErrReadOnly = -32003, kErrFault = -32004,
};
enum Close : uint16_t {
    kCloseKicked = 4000, kCloseProtocol = 4001, kCloseNoHello = 4002, kCloseAuth = 4003, kCloseQueue = 4008,
    kCloseTooMany = 4029,
};

struct Host {
    std::string server = "game";   // "game" | "standalone"
    std::string gameJson;          // welcome.game: a JSON object, or "" for none
    std::vector<std::string> caps; // welcome.caps, e.g. "launcher"
    uint64_t (*frame)() = nullptr; // sys.ping's frame counter
};
void SetHost(const Host& h);
// Inside an RPC handler: a binary frame to send right after the handler's result (announced by a `bin` message; the
// frame starts with `ref`, u32 LE). Dropped when the handler fails. Any thread.
void QueueBinary(uint32_t ref, std::string_view ch, std::string_view metaJson, std::string_view bytes);
void SetBuild(std::string build);  // the web bundle's build id, sent in welcome
std::string Build();

namespace router {
void Configure(const Config& c);
bool ReadOnly();   // [Oasis] ReadOnly as configured; any thread
// Server side of one WebSocket connection. Open returns the client id; the client may already be marked for
// closing (too many clients). `wake` is signalled whenever the client has something to send.
int Open(void* wake);
void Text(int id, std::string_view msg);
// The writer's view: due messages, the wait until the next batch, and a close to perform (code 0 = none).
// False once the client is gone.
bool Take(int id, uint32_t nowMs, std::vector<std::string>* out, uint32_t* waitMs, uint16_t* closeCode,
          std::string* closeReason, std::vector<uint8_t>* binary = nullptr);
void Close(int id, uint16_t code, const char* reason);  // queue a close; any thread
void Gone(int id);                                       // the connection ended; drops subscriptions
void Release(int id);                                    // the writer has exited; frees the client
void CloseAll(uint16_t code, const char* reason);
void CountIo(uint64_t framesOut, uint64_t bytesOut, uint64_t bytesIn);
void CountAuthFailure();
int OpenClients();
std::vector<int> ListClients();  // A: connected (helloed) client ids, for the overlay panel's kick list
}  // namespace router
}  // namespace melange::oasis::core
