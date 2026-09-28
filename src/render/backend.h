#pragma once
// Dear ImGui context + Win32/OpenGL2 backend lifetime for the overlay (component A). Main thread only, with the
// game's GL context current. Kept separate from overlay.cpp so the offline test runner can exercise the
// context-reset path (Shutdown(true) + Init on a new context) without the game.
#include <string>

namespace melange::render::backend {
// Creates the ImGui context on first use (io.IniFilename = iniUtf8, nullptr/empty = no ini; onCreate runs once
// right after creation, before any frame, e.g. to register settings handlers), then initialises the Win32 and
// OpenGL2 backends for `hwnd`. False if a backend failed.
bool Init(void* hwnd, const char* iniUtf8, void (*onCreate)() = nullptr);
// Shuts both backends down. contextLost = the GL context the ImGui textures live in is gone (or not current):
// their ids are forgotten without glDeleteTextures, and they are re-created on the next render.
void Shutdown(bool contextLost);
bool Ready();
void* Hwnd();
}  // namespace melange::render::backend
