#pragma once
// Mod folders: <game>\<ModsDir>\<id>\, priority by folder name (a later name wins).
#include <string>

namespace melange::mirage::modfs {
struct Root { const char* id; const wchar_t* dir; };
int Roots(Root* out, int max);  // enabled mods, lowest priority first
// Highest-priority file for relPath (e.g. L"shaders\\Landscape.cg"); empty string if none. *owner = mod id.
std::wstring Resolve(const wchar_t* relPath, const char** owner);
using ChangeFn = void (*)(const wchar_t* path, void* user);  // main thread, debounced 300 ms
int Watch(const wchar_t* subdir, ChangeFn fn, void* user);   // one ReadDirectoryChangesW thread for all mods

// Called by the Mirage module.
void Configure(const std::wstring& modsDir, const std::string& disabled);
void OnFrame();
}
