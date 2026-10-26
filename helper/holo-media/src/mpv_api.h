#pragma once
// libmpv, loaded at run time by full path (--libmpv): the pack's verified libmpv-2.dll, or the user's own under
// [Media] AllowCustomLibmpv. Nothing links against it; the headers come from the pinned dev zip (HOLO_MPV_DIR).
#if HOLO_HAVE_MPV
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/stream_cb.h>

#include <string>

namespace holo::mpv {
#define HOLO_MPV_FUNCS(X)                                                                                              \
    X(mpv_client_api_version) X(mpv_error_string) X(mpv_free) X(mpv_create) X(mpv_initialize) X(mpv_terminate_destroy) \
    X(mpv_set_option) X(mpv_set_option_string) X(mpv_set_property) X(mpv_set_property_async) X(mpv_get_property)      \
    X(mpv_get_property_string) X(mpv_free_node_contents) X(mpv_command_node_async) X(mpv_command_async)               \
    X(mpv_observe_property) X(mpv_wait_event) X(mpv_set_wakeup_callback) X(mpv_request_log_messages)                  \
    X(mpv_event_name) X(mpv_render_context_create) X(mpv_render_context_set_update_callback)                         \
    X(mpv_render_context_update) X(mpv_render_context_render) X(mpv_render_context_free) X(mpv_stream_cb_add_ro)

struct Api {
#define HOLO_MPV_PTR(f) decltype(&::f) f = nullptr;
    HOLO_MPV_FUNCS(HOLO_MPV_PTR)
#undef HOLO_MPV_PTR
};

// Loads the library once (SetDefaultDllDirectories is already in force: system32 and the DLL's own folder only).
// False with a reason; the helper then answers every `open` with error{code="load"}.
bool Load(const std::wstring& path, std::string* why);
const Api& A();
}  // namespace holo::mpv
#endif
