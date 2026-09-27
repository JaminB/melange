// Stub for component A (render/overlay core and input). A replaces this file with the real ImGui/GL2 backend
// of docs/m0-design.md SS3 "A".
//
// NOTE (added by component C, 2026-09-27): same situation as src/core/bus.cpp - the original stub had no
// function bodies, so anything built against wumfix/overlay.h (component C's log_viewer.cpp registers the
// "Log" and "Events" panels here) could not link. Every function below is a neutral stand-in: AddPanel/
// AddMenuItem/AddHotkey fail (-1), Installed()/Visible()/Capturing() are false, Gl()/GetStats() are default-
// constructed. No ImGui context is created and no GL state is touched. This is not component A's real
// behaviour and is expected to be discarded when A replaces this file wholesale.
#include "wumfix/overlay.h"

namespace wf::overlay {

int AddPanel(const char* /*id*/, const char* /*title*/, DrawFn /*fn*/, void* /*user*/, uint32_t /*flags*/) { return -1; }
void RemovePanel(int /*handle*/) {}
int AddMenuItem(const char* /*path*/, ActionFn /*fn*/, void* /*user*/, const char* /*shortcut*/) { return -1; }
int AddHotkey(uint8_t /*dik*/, uint8_t /*mods*/, ActionFn /*fn*/, void* /*user*/) { return -1; }
bool ParseHotkey(const char* /*text*/, uint8_t* /*dik*/, uint8_t* /*mods*/) { return false; }

bool Visible() { return false; }
void SetVisible(bool /*v*/) {}
bool Capturing() { return false; }
void SetCapture(bool /*on*/) {}

GlInfo Gl() { return GlInfo{}; }
Stats GetStats() { return Stats{}; }
bool Installed() { return false; }

}  // namespace wf::overlay
