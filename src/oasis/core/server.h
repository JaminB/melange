#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "oasis/core/http.h"

// The Oasis server core: civetweb on 127.0.0.1, the protocol router and the per-client writers. No game
// dependency; linked by melange.asi and oasis.exe.
namespace melange::oasis::core {
struct Config {
    int port = 8765, portRange = 10, maxClients = 4;
    uint32_t maxQueueKB = 2048, maxMessageKB = 1024, maxHeaderKB = 16;
    bool readOnly = false;
};
class Auth {
  public:
    virtual ~Auth() = default;
    // Every HTTP request (the upgrade included) after Validate(). False: send *denyOrRedirect instead.
    virtual bool CheckHttp(const struct Request& rq, struct Response* denyOrRedirect) = 0;
    // The /ws upgrade, after CheckHttp. False: 403.
    virtual bool CheckUpgrade(const Request& rq) = 0;
    virtual std::string LaunchUrl(int port) const = 0;
};
class Files {  // static content: embedded zip (asi) or folder (WebRoot, oasis.exe)
  public:
    virtual ~Files() = default;
    // `path` has no leading '/' and passed SafePath(). On input *gzipped says whether the client accepts gzip;
    // on output whether *body is gzip-encoded. *etag is a quoted strong ETag.
    virtual bool Get(std::string_view path, std::string* body, std::string* etag, bool* gzipped) = 0;
};
bool Start(const Config& c, Auth* a, Files* f);  // spawns the server threads
void Stop();                                     // oasis.exe and tests only; the asi never needs it
int Port();
using MainPump = void (*)();                     // the host calls Pump() once per frame (Frame event / exe loop)
void Pump();                                     // runs queued main-thread RPCs (budgeted) and subscribe callbacks

bool Running();                                  // any thread
std::string LaunchUrl();                         // "" when not running; any thread
// Extra authenticated GET/HEAD routes (captures, logs, web panels). Runs on a server thread; return false when
// the path is not yours (404). Prefixes start and end with '/'.
using Route = bool (*)(const Request& rq, Response* out, void* user);
int AddRoute(const char* prefix, Route fn, void* user);
void RemoveRoute(int handle);
void NoteAuthFailure();                          // counted in Stats.authFailures; any thread
void Kick(int client = 0);                       // close one client (0 = all) with 4000; any thread
}  // namespace melange::oasis::core
