#pragma once
#include <cstdint>
#include <functional>

// Game-wide events modules can subscribe to. The core installs the hooks that fire them.
namespace wf::events {
enum class Event {
    Frame,       // once per presented frame (gdi32!SwapBuffers), on the main thread
    Shutdown,    // process is exiting
    MatchStart,  // fired by modules that detect online match boundaries (see netsession)
    MatchEnd,
    LobbyEnter,
    LobbyLeave,
    Count
};

using Callback = std::function<void()>;
void Subscribe(Event e, Callback cb);
void Fire(Event e);
const char* NameOf(Event e);

void InstallCore();  // frame hook etc.

// Main-thread heartbeat info (valid after the first frame).
uint64_t FrameCount();
uint64_t LastFrameTick();  // GetTickCount64() of the last frame
unsigned long MainThreadId();
void* GameWindow();  // HWND
}  // namespace wf::events
