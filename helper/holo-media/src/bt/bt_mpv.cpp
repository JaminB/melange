// The holobt:// stream callbacks for mpv (bt_mpv.h).
#if HOLO_HAVE_MPV
#include "bt/bt_mpv.h"

#include <string>

#include "bt/bt_logic.h"

namespace holo::bt {
namespace {
Stream* S(void* cookie) { return static_cast<Stream*>(cookie); }

int64_t ReadFn(void* cookie, char* buf, uint64_t n) {
    try {
        return S(cookie)->Read(buf, n);
    } catch (...) {
        return -1;
    }
}
int64_t SeekFn(void* cookie, int64_t offset) {
    try {
        return S(cookie)->Seek(offset) ? offset : MPV_ERROR_GENERIC;
    } catch (...) {
        return MPV_ERROR_GENERIC;
    }
}
int64_t SizeFn(void* cookie) {
    try {
        const int64_t n = S(cookie)->Size();
        return n >= 0 ? n : MPV_ERROR_UNSUPPORTED;
    } catch (...) {
        return MPV_ERROR_UNSUPPORTED;
    }
}
void CancelFn(void* cookie) {
    try {
        S(cookie)->Cancel();
    } catch (...) {
    }
}
void CloseFn(void* cookie) { delete S(cookie); }
}  // namespace

int MpvStreamOpen(void* engine, char* uri, mpv_stream_cb_info* info) {
    if (!engine || !uri || !info) return MPV_ERROR_LOADING_FAILED;
    std::string ih;
    int index = 0;
    if (!logic::ParseStreamUrl(uri, &ih, &index)) return MPV_ERROR_LOADING_FAILED;
    std::string why;
    std::unique_ptr<Stream> s;
    try {
        s = static_cast<Engine*>(engine)->Open(ih, index, &why);
    } catch (...) {
        s.reset();
    }
    if (!s) return MPV_ERROR_LOADING_FAILED;
    info->cookie = s.release();
    info->read_fn = ReadFn;
    info->seek_fn = SeekFn;
    info->size_fn = SizeFn;
    info->close_fn = CloseFn;
    info->cancel_fn = CancelFn;
    return 0;
}

int RegisterMpvStream(mpv_handle* mpv, Engine* engine, MpvStreamCbAddRo addRo) {
    if (!engine) return 0;
    if (!mpv || !addRo) return MPV_ERROR_INVALID_PARAMETER;
    return addRo(mpv, kStreamProtocol, engine, MpvStreamOpen);
}
}  // namespace holo::bt
#endif
