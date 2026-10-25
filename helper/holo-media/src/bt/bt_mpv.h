#pragma once
// mpv reads torrent files through a stream callback (mpv_stream_cb_add_ro), with no loopback HTTP server: the
// "holobt://<infohash>/<index>" protocol of bt.h, served by Engine::Open and the Stream it returns. Built only with
// the libmpv headers (HOLO_HAVE_MPV).
//
// Use (C6a): after mpv_initialize, RegisterMpvStream(mpv, engine, pfn_mpv_stream_cb_add_ro), then
// loadfile bt::StreamUrl(ih, index) with ytdl=no. mpv calls the open callback on its own thread, so the engine's
// Open must be safe there: the libtorrent engine's is (it locks its own state). The engine must outlive the mpv
// handle (terminate mpv before Engine::Shutdown). Streams still open when a torrent is removed are cancelled by the
// engine and fail their reads; mpv's close callback frees them.
#if HOLO_HAVE_MPV
#include <mpv/client.h>
#include <mpv/stream_cb.h>

#include "bt/bt.h"

namespace holo::bt {
using MpvStreamCbAddRo = int (*)(mpv_handle*, const char*, void*, mpv_stream_cb_open_ro_fn);

// Registers kStreamProtocol on this mpv handle. Returns mpv's error code (0 = success, also when engine is null:
// then nothing is registered and holobt:// URLs fail to load like any unknown protocol).
int RegisterMpvStream(mpv_handle* mpv, Engine* engine, MpvStreamCbAddRo addRo);

// The open callback itself, for tests: fills `info` for a valid holobt:// URI the engine can open.
int MpvStreamOpen(void* engine, char* uri, mpv_stream_cb_info* info);
}  // namespace holo::bt
#endif
