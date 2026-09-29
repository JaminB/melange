#pragma once
#include <cstdint>

// Additive to capture.h: a sink the recorder registers to observe every sender and insert call capture.cpp
// already hooks, without installing a second set of hooks. capture.h's own public surface is unchanged.
namespace melange::wormsign::capture {
struct SendEvent {
    int type;
    uint16_t id;
    uint32_t a, b, time, callT, caller;
    const char* str;  // "" when none; valid only for the duration of the callback
    bool injected;
};
struct InsertEvent {
    uint16_t id;
    uint32_t time, arrivedT, a;
};
using SendSink = void (*)(const SendEvent&, void* user);
using InsertSink = void (*)(const InsertEvent&, void* user);
void SetSendSink(SendSink fn, void* user);      // one owner (the recorder); main thread only
void SetInsertSink(InsertSink fn, void* user);
bool Install();                                  // the six sender + five insert hooks
bool Installed();
}  // namespace melange::wormsign::capture
