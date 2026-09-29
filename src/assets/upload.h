#pragma once
#include <cstdint>

// The texture-upload tap (0x79dc50): registered patchers see an XImage's pixels just before its GL upload.
namespace melange::assets::upload {
using PatchFn = void (*)(const char* name, uint16_t w, uint16_t h, uint32_t fmt, uint8_t* rgb, uint32_t size, void*);
// imageName matches the XImage name with or without its extension, case-insensitively. The hook is enabled only while
// a patcher exists. Returns a handle (0 on failure).
int AddPatcher(const char* imageName, PatchFn fn, void* user);
void RemovePatcher(int handle);
struct Stats { uint32_t patchers, uploadsSeen, uploadsPatched; double msLastPatch; bool hooked; };
Stats GetStats();
bool Available();  // the site has its #1077 bytes
}  // namespace melange::assets::upload
