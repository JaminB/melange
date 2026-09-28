#pragma once
// Dear ImGui context and Win32/OpenGL2 backend lifetime. Main thread only, with the game's GL context current.
#include <string>

namespace melange::render::backend {
// Creates the ImGui context on first use (empty iniUtf8 = no imgui.ini; onCreate runs once after creation),
// then initialises both backends for `hwnd`.
bool Init(void* hwnd, const char* iniUtf8, void (*onCreate)() = nullptr);
// contextLost: the textures' GL context is gone, so forget their ids instead of deleting them.
void Shutdown(bool contextLost);
bool Ready();
void* Hwnd();
}  // namespace melange::render::backend
