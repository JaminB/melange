#include "mpv_api.h"

#if HOLO_HAVE_MPV
#include <windows.h>

#include "util.h"

namespace holo::mpv {
namespace {
Api g_api;
HMODULE g_lib = nullptr;
}  // namespace

bool Load(const std::wstring& path, std::string* why) {
    if (g_lib) return true;
    // Only the DLL's own folder and system32: never the current directory or PATH.
    HMODULE lib = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) {
        if (why) *why = "LoadLibrary: " + WinError(GetLastError());
        return false;
    }
    Api a;
#define HOLO_MPV_GET(f)                                                                    \
    a.f = reinterpret_cast<decltype(a.f)>(reinterpret_cast<void*>(GetProcAddress(lib, #f))); \
    if (!a.f) {                                                                            \
        if (why) *why = "libmpv has no " #f;                                               \
        FreeLibrary(lib);                                                                  \
        return false;                                                                      \
    }
    HOLO_MPV_FUNCS(HOLO_MPV_GET)
#undef HOLO_MPV_GET
    // Client API 2.x: the render, stream_cb and loadfile-with-options calls used here.
    const unsigned long v = a.mpv_client_api_version();
    if ((v >> 16) != 2 || (v & 0xFFFF) < 1) {
        if (why) *why = "libmpv client API " + std::to_string(v >> 16) + "." + std::to_string(v & 0xFFFF) + " (need 2.1+)";
        FreeLibrary(lib);
        return false;
    }
    g_api = a;
    g_lib = lib;
    return true;
}

const Api& A() { return g_api; }
}  // namespace holo::mpv
#endif
